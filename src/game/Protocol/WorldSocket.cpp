/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include "Auth/AuthCrypt.h"

#include "World.h"
#include "AccountMgr.h"
#include "SharedDefines.h"
#include "WorldSession.h"
#include "WorldSocket.h"
#include "WorldSocketMgr.h"
#include "AddonHandler.h"
#include "Anticheat/Anticheat.h"


#include "Opcodes.h"
#include "MangosSocketImpl.h"

#include <memory>

template class MangosSocket<WorldSession, WorldSocket, AuthCrypt>;



int WorldSocket::ProcessIncoming(WorldPacket* new_pct)
{
    MANGOS_ASSERT(new_pct);


    // manage memory ;)
    std::unique_ptr<WorldPacket> aptr(new_pct);

    const ACE_UINT16 opcode = new_pct->GetOpcode();

    if (opcode >= NUM_MSG_TYPES)
    {
        sLog.outError("SESSION: received nonexistent opcode 0x%.4X", opcode);
        return -1;
    }

    if (closing_)
        return -1;

    new_pct->FillPacketTime(WorldTimer::getMSTime());

    // Dump received packet.
    sLog.outWorldPacketDump(get_handle(), new_pct->GetOpcode(),
                            LookupOpcodeName(new_pct->GetOpcode()), new_pct,
                            true);

    try
    {
        switch (opcode)
        {
            case CMSG_PING:
                return HandlePing(*new_pct);
            case CMSG_AUTH_SESSION:
                if (m_Session)
                {
                    sLog.outError("WorldSocket::ProcessIncoming: Player send CMSG_AUTH_SESSION again");
                    return -1;
                }

                return HandleAuthSession(*new_pct);
            default:
            {
                GuardType lock(m_SessionLock);

                if (m_Session != nullptr)
                {
                    // OK ,give the packet to WorldSession
                    aptr.release();
                    // WARNINIG here we call it with locks held.
                    // Its possible to cause deadlock if QueuePacket calls back
                    m_Session->QueuePacket(new_pct);
                    return 0;
                }
                else
                {
                    sLog.outError("WorldSocket::ProcessIncoming: Client not authed opcode = %u", uint32(opcode));
                    return -1;
                }
            }
        }
    }
    catch (ByteBufferException &)
    {
        sLog.outError("WorldSocket::ProcessIncoming ByteBufferException occured while parsing an instant handled packet (opcode: %u) from client %s, accountid=%i.",
                      opcode, GetRemoteAddress().c_str(), m_Session ? m_Session->GetAccountId() : -1);
        if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        {
            DEBUG_LOG("Dumping error-causing packet:");
            new_pct->hexlike();
        }

        if (sWorld.getConfig(CONFIG_BOOL_KICK_PLAYER_ON_BAD_PACKET))
        {
            DETAIL_LOG("Disconnecting session [account id %i / address %s] for badly formatted packet.",
                       m_Session ? m_Session->GetAccountId() : -1, GetRemoteAddress().c_str());

            return -1;
        }
        else
            return 0;
    }

    ACE_NOTREACHED(return 0);
}

int WorldSocket::HandleAuthSession(WorldPacket& recvPacket)
{
    // NOTE: ATM the socket is singlethread, have this in mind ...
    uint8 digest[20];
    uint32 clientSeed;
    uint32 serverId;
    uint32 BuiltNumberClient;
    uint32 id, security;
    LocaleConstant locale;
    std::string account, os, platform;
    BigNumber v, s, g, N, K;
    WorldPacket packet, SendAddonPacked;

    // Read the content of the packet
    recvPacket >> BuiltNumberClient;
    recvPacket >> serverId;
    recvPacket >> account;

    recvPacket >> clientSeed;
    recvPacket.read(digest, 20);

    DEBUG_LOG("WorldSocket::HandleAuthSession: client %u, serverId %u, account %s, clientseed %u",
              BuiltNumberClient,
              serverId,
              account.c_str(),
              clientSeed);

    // Check the version of client trying to connect
    if (!IsAcceptableClientBuild(BuiltNumberClient))
    {
        packet.Initialize(SMSG_AUTH_RESPONSE, 1);
        packet << uint8(AUTH_VERSION_MISMATCH);

        SendPacket(packet);

        sLog.outError("WorldSocket::HandleAuthSession: Sent Auth Response (version mismatch).");
        return -1;
    }

    // Get the account information from the realmd database
    std::string safe_account = account; // Duplicate, else will screw the SHA hash verification below
    LoginDatabase.escape_string(safe_account);
    // No SQL injection, username escaped.

	QueryResult* result = LoginDatabase.PQuery("SELECT a.id, a.rank, a.sessionkey, a.last_ip, a.locked, a.v, a.s, a.mutetime, a.locale, a.os, a.platform, a.flags, a.email, a.username, UNIX_TIMESTAMP(a.joindate), a.queue_skip, "
		"ab.unbandate > UNIX_TIMESTAMP() OR ab.unbandate = ab.bandate FROM account a "
		"LEFT JOIN account_banned ab ON a.id = ab.id AND ab.active = 1 WHERE a.username = '%s' LIMIT 1", safe_account.c_str());

    // Stop if the account is not found
    if (!result)
    {
        packet.Initialize(SMSG_AUTH_RESPONSE, 1);
        packet << uint8(AUTH_UNKNOWN_ACCOUNT);

        SendPacket(packet);

        sLog.outError("WorldSocket::HandleAuthSession: Sent Auth Response (unknown account).");
        return -1;
    }

    Field* fields = result->Fetch();

    N.SetHexStr("894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7");
    g.SetDword(7);

    v.SetHexStr(fields[5].GetString());
    s.SetHexStr(fields[6].GetString());

    const char* sStr = s.AsHexStr();                        //Must be freed by OPENSSL_free()
    const char* vStr = v.AsHexStr();                        //Must be freed by OPENSSL_free()

    DEBUG_LOG("WorldSocket::HandleAuthSession: (s,v) check s: %s v: %s",
              sStr,
              vStr);

    OPENSSL_free((void*) sStr);
    OPENSSL_free((void*) vStr);

    auto const remote_ip = fields[3].GetCppString();

    ///- Re-check ip locking (same check as in realmd).

    //This should always be checked regardless of IP locking.
    //If the last_ip that was just modified by authserver is different than the client sending CMSG_AUTH_SESSION that's never okay.

   /*if (strcmp(remote_ip.c_str(), GetRemoteAddress().c_str()))
    {
        packet.Initialize(SMSG_AUTH_RESPONSE, 1);
        packet << uint8(AUTH_FAILED);
        SendPacket(packet);

        delete result;
        BASIC_LOG("WorldSocket::HandleAuthSession: Sent Auth Response (Account IP differs).");
        return -1;
    }*/

    id = fields[0].GetUInt32();
    security = sAccountMgr.GetSecurity(id); //fields[1].GetUInt16 ();
    if (security > SEC_SIGMACHAD)                       // prevent invalid security settings in DB
        security = SEC_SIGMACHAD;

    if (sAccountMgr.IsTraineeGM(id))
        security = SEC_DEVELOPER;

    auto str = fields[2].GetString();

    K.SetHexStr(fields[2].GetString());

    auto vec = K.AsByteArray();

    if (K.AsByteArray().empty())
    {
        delete result;
        return -1;
    }

    time_t mutetime = time_t (fields[7].GetUInt64());

    locale = LocaleConstant(fields[8].GetUInt8());
    if (locale >= MAX_LOCALE)
        locale = LOCALE_enUS;
    os = fields[9].GetCppString();
    platform = fields[10].GetCppString();
    uint32 accFlags = fields[11].GetUInt32();
    std::string email = fields[12].GetCppString();
    std::string username = fields[13].GetCppString();
    uint32 joinTimestamp = fields[14].GetUInt32();
    bool canQueueSkip = fields[15].GetBool();
    bool isBanned = fields[16].GetBool();
    delete result;

    
    if (isBanned || sAccountMgr.IsIPBanned(GetRemoteAddress()))
    {
        packet.Initialize(SMSG_AUTH_RESPONSE, 1);
        packet << uint8(AUTH_BANNED);
        SendPacket(packet);

        sLog.outError("WorldSocket::HandleAuthSession: Sent Auth Response (Account banned).");
        return -1;
    }

    // Check locked state for server
    AccountTypes allowedAccountType = sWorld.GetPlayerSecurityLimit();

    if (allowedAccountType > SEC_PLAYER && AccountTypes(security) < allowedAccountType)
    {
        WorldPacket Packet(SMSG_AUTH_RESPONSE, 1);
        Packet << uint8(AUTH_UNAVAILABLE);

        SendPacket(packet);

        BASIC_LOG("WorldSocket::HandleAuthSession: User tries to login but his security level is not enough");
        return -1;
    }

    // Check that Key and account name are the same on client and server
    Sha1Hash sha;

    uint32 t = 0;
    uint32 seed = m_Seed;

    sha.UpdateData(account);
    sha.UpdateData((uint8 *) & t, 4);
    sha.UpdateData((uint8 *) & clientSeed, 4);
    sha.UpdateData((uint8 *) & seed, 4);
    sha.UpdateBigNumbers(&K, nullptr);
    sha.Finalize();

    if (memcmp(sha.GetDigest(), digest, 20))
    {
        packet.Initialize(SMSG_AUTH_RESPONSE, 1);
        packet << uint8(AUTH_FAILED);

        SendPacket(packet);

        sLog.outError("WorldSocket::HandleAuthSession: Sent Auth Response (authentification failed).");
        return -1;
    }

    std::string address = GetRemoteAddress();

    DEBUG_LOG("WorldSocket::HandleAuthSession: Client '%s' authenticated successfully from %s.",
              account.c_str(),
              address.c_str());

    ClientOSType clientOs;
    if (os == "niW")
        clientOs = CLIENT_OS_WIN;
    else if (os == "XSO")
        clientOs = CLIENT_OS_MAC;
    else
    {
        sLog.outError("WorldSocket::HandleAuthSession: Unrecognized OS '%s' for account '%s' from %s", os.c_str(), account.c_str(), address.c_str());
        return -1;
    }

    ClientPlatformType clientPlatform;
    if (platform == "68x")
        clientPlatform = CLIENT_PLATFORM_X86;
    else if (platform == "CPP" && clientOs == CLIENT_OS_MAC)
        clientPlatform = CLIENT_PLATFORM_PPC;
    else
    {
        sLog.outError("WorldSocket::HandleAuthSession: Unrecognized Platform '%s' for account '%s' from %s", platform.c_str(), account.c_str(), address.c_str());
        return -1;
    }

    // NOTE ATM the socket is single-threaded, have this in mind ...
    ACE_NEW_RETURN(m_Session, WorldSession(id, this, AccountTypes(security), mutetime, locale, remote_ip, m_BinaryAddress), -1);

    m_Crypt.SetKey(K.AsByteArray());
    m_Crypt.Init();

    m_Session->SetShouldBackupCharacters(sAccountMgr.UpdateAccountIP(id, GetRemoteAddress()));
    m_Session->SetJoinTimeStamp(joinTimestamp);
    m_Session->SetUsername(account);
    m_Session->SetEmail(email);
    m_Session->SetGameBuild(BuiltNumberClient);
    m_Session->SetAccountFlags(accFlags);
    m_Session->SetOS(clientOs);
    m_Session->SetPlatform(clientPlatform);
    m_Session->LoadTutorialsData();
    m_Session->SetQueueSkip(canQueueSkip);
    m_Session->InitAntiCheatSession(&K);


    //TWoW Jamey
    //Clear out sessionkey once we're done with it. Keep it in memory only.
    //Ideally sessionkey shouldn't even be in the DB. It's only used for auth to communicate to world.
    //Exposing it leaves a big security hole as it allows free login.
    //Should be sent over IPC / MMAP. 


    //LoginDatabase.DirectPExecute("UPDATE `account` SET `sessionkey` = '' WHERE `username` = '%s'", safe_account.c_str());

    //m_Session->InitWarden(&K);

    // In case needed sometime the second arg is in microseconds 1 000 000 = 1 sec
    ACE_OS::sleep(ACE_Time_Value(0, 10000));

    // just refresh always..
    auto accountData = sWorld.GetAccountData(id);
    accountData->id = id;
    accountData->email = email;
    accountData->username = username;

    sWorld.AddSession(m_Session);

    // when false, the client sent invalid addon data.  kick!
    WorldPacket addonPacket;
    if (!m_Session->GetAntiCheat()->ReadAddonInfo(&recvPacket, addonPacket))
    {
        sLog.out(LOG_ANTICHEAT_BASIC, "WorldSocket::HandleAuthSession: Account %s (id %u) IP %s sent bad addon info.  Kicking.",
            account.c_str(), id, GetRemoteAddress().c_str());
        return -1;
    }

    // if anything was written to the packet, send it
    if (addonPacket.wpos())
        SendPacket(addonPacket);

    return 0;
}

int WorldSocket::HandlePing(WorldPacket& recvPacket)
{
    uint32 ping;
    uint32 latency;

    // Get the ping packet content
    recvPacket >> ping;
    recvPacket >> latency;

    if (m_LastPingTime == ACE_Time_Value::zero)
        m_LastPingTime = ACE_OS::gettimeofday();  // for 1st ping
    else
    {
        ACE_Time_Value cur_time = ACE_OS::gettimeofday();
        ACE_Time_Value diff_time(cur_time);
        diff_time -= m_LastPingTime;
        m_LastPingTime = cur_time;

        if (diff_time < ACE_Time_Value(27))
        {
            ++m_OverSpeedPings;

            uint32 max_count = sWorld.getConfig(CONFIG_UINT32_MAX_OVERSPEED_PINGS);

            if (max_count && m_OverSpeedPings > max_count)
            {
                GuardType lock(m_SessionLock);

                if (m_Session && m_Session->GetSecurity() == SEC_PLAYER)
                {
                    sLog.outError("WorldSocket::HandlePing: Player kicked for "
                                  "overspeeded pings address = %s",
                                  GetRemoteAddress().c_str());

                    return -1;
                }
            }
        }
        else
            m_OverSpeedPings = 0;
    }

    // critical section
    {
        GuardType lock(m_SessionLock);

        if (m_Session)
            m_Session->SetLatency(latency);
        else
        {
            sLog.outError("WorldSocket::HandlePing: peer sent CMSG_PING, "
                          "but is not authenticated or got recently kicked,"
                          " address = %s",
                          GetRemoteAddress().c_str());
            return -1;
        }
    }

    WorldPacket packet(SMSG_PONG, 4);
    packet << ping;
    return SendPacket(packet);
}

int WorldSocket::OnSocketOpen()
{
    return sWorldSocketMgr->OnSocketOpen(this);
}

int WorldSocket::SendStartupPacket()
{
    // Send startup packet.
    WorldPacket packet(SMSG_AUTH_CHALLENGE, 4);
    packet << m_Seed;

    return SendPacket(packet);
}
