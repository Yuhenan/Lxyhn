/*
 * Copyright (C) 2016 Elysium Project <https://elysium-project.org>
 */

#include "Formulas.h"
#include "HonorMgr.h"
#include "Language.h"
#include "World.h"
#include "Creature.h"
#include "Player.h"
#include "Database/DatabaseEnv.h"
#include "Policies/SingletonImp.h"
#include "ObjectAccessor.h"
#include "CharacterDatabaseCleaner.h"

#include <fstream>

#define FAKE_PVP_POOL 800

HonorMaintenancer sHonorMaintenancer;

HonorStandingList& HonorMaintenancer::GetStandingListByTeam(Team team)
{
    switch (team)
    {
        case ALLIANCE:
            return m_allianceStandingList;
        case HORDE:
            return m_hordeStandingList;
        default:
            return m_allianceStandingList;
    }
}

float HonorMaintenancer::GetStandingCPByPosition(HonorStandingList& standingList, uint32 position)
{
    uint32 pos = 1;
    for (auto& standing : standingList)
    {
        if (pos == position)
            return standing.cp;
        pos++;
    }

    return 0.0f;
}

uint32 HonorMaintenancer::GetStandingPositionByGUID(uint32 guid, Team team)
{
    uint32 pos = 1;
    auto& standingList = GetStandingListByTeam(team);
    for (auto& standing : standingList)
    {
        if (standing.guid == guid)
            return pos;
        pos++;
    }

    return 0;
}

uint32 HonorMgr::m_mostHkYesterdayGuid = 0;
uint32 HonorMgr::m_mostDkYesterdayGuid = 0;

void HonorMgr::LoadMostDkHkYesterdayPlayers()
{
    sLog.outInfo("Loading players that had the most HK and DK yesterday.");

    uint32 maxDay = 0;
    std::map<uint32 /*guid*/, uint32 /*hk*/> hkRanking;
    std::map<uint32 /*guid*/, uint32 /*dk*/> dkRanking;
    QueryResult* result = CharacterDatabase.Query("SELECT `guid`, `type`, `date` FROM `character_honor_cp`");
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            
            uint32 guid = fields[0].GetUInt32();
            uint32 type = fields[1].GetUInt32();
            uint32 date = fields[2].GetUInt32();

            if (date > maxDay)
            {
                maxDay = date;
                hkRanking.clear();
                dkRanking.clear();
            }
            else if (date < maxDay)
                continue;

            if (type == HONORABLE)
            {
                hkRanking[guid]++;
            }
            else if (type == DISHONORABLE)
            {
                dkRanking[guid]++;
            }

        } while (result->NextRow());
        delete result;
    }

    std::pair<uint32, uint32> mostHk;
    for (auto const& itr : hkRanking)
    {
        if (itr.second > mostHk.second)
        {
            mostHk.first = itr.first;
            mostHk.second = itr.second;
        }
    }
    m_mostHkYesterdayGuid = mostHk.first;

    std::pair<uint32, uint32> mostDk;
    for (auto const& itr : dkRanking)
    {
        if (itr.second > mostDk.second)
        {
            mostDk.first = itr.first;
            mostDk.second = itr.second;
        }
    }
    m_mostDkYesterdayGuid = mostDk.first;

    sLog.outInfo("Most HK by %u, Most DK by %u", m_mostHkYesterdayGuid, m_mostDkYesterdayGuid);

    if (m_mostHkYesterdayGuid)
    {
        if (PlayerCacheData* pData = sObjectMgr.GetPlayerDataByGUID(m_mostHkYesterdayGuid))
        {
            if (Quest* pQuest = (Quest*)sObjectMgr.GetQuestTemplate(QUEST_DAILY_MOST_HK))
            {
                std::string txt = "End " + pData->sName + "'s life.";
                pQuest->SetEndText(txt);
            }
        }
    }

    if (m_mostDkYesterdayGuid)
    {
        if (PlayerCacheData* pData = sObjectMgr.GetPlayerDataByGUID(m_mostDkYesterdayGuid))
        {
            if (Quest* pQuest = (Quest*)sObjectMgr.GetQuestTemplate(QUEST_DAILY_MOST_DK))
            {
                std::string txt = "End " + pData->sName + "'s life.";
                pQuest->SetEndText(txt);
            }
        }
    }
}

void HonorMaintenancer::LoadWeeklyScores()
{
    uint32 weekBeginDay = GetWeekBeginDay();
    uint32 weekEndDay = GetWeekEndDay();

    std::ostringstream query;

    query << "SELECT `scores`.`guid`, `c`.`level`, `c`.`account`, `c`.`honorRankPoints`, `c`.`honorHighestRank`, SUM(`hk`), SUM(`dk`), SUM(`cp`) FROM"
        "("
        "  SELECT `guid` AS `guid`, COUNT(*) AS `hk`, 0 AS `dk`, SUM(`cp`) AS `cp` FROM `character_honor_cp` WHERE `type` = " << HONORABLE <<
        "  AND (`date` BETWEEN " << weekBeginDay << " AND " << weekEndDay << ") GROUP BY `guid`"
        "  UNION"
        "  SELECT `guid` AS `guid`, 0 AS `hk`, COUNT(*) AS `dk`, 0 AS `cp` FROM `character_honor_cp` WHERE `type` = " << DISHONORABLE <<
        "  AND (`date` BETWEEN " << weekBeginDay << " AND " << weekEndDay << ") GROUP BY `guid`"
        "  UNION"
        "  SELECT `guid` AS `guid`, 0 AS `hk`, 0 AS `dk`, SUM(`cp`) AS `cp` FROM `character_honor_cp` WHERE `type` NOT IN (" << HONORABLE << ", " << DISHONORABLE << ")"
        "  AND (`date` BETWEEN " << weekBeginDay << " AND " << weekEndDay << ") GROUP BY `guid`"
        "  UNION"
        "  SELECT `guid` AS `guid`, 0 AS `hk`, 0 AS `dk`, 0 AS `cp` FROM `characters` WHERE `honorRankPoints` > 0"
        ") AS `scores` INNER JOIN `characters` AS `c` ON `scores`.`guid` = `c`.`guid` GROUP BY `guid` ORDER BY `guid` ";

    QueryResult* result = CharacterDatabase.Query(query.str().c_str());

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            WeeklyScore score;
            score.level  = fields[1].GetUInt32();
            score.account = fields[2].GetUInt32();
            score.oldRp  = fields[3].GetFloat();
            score.highestRank = fields[4].GetUInt32();
            score.hk  = fields[5].GetUInt32();
            score.dk  = fields[6].GetUInt32();
            score.cp  = fields[7].GetFloat();
            m_weeklyScores[fields[0].GetUInt32()] = score;
        }
        while (result->NextRow());
        delete result;
    }
}

void HonorMaintenancer::LoadStandingLists()
{
    uint8 minHK = sWorld.getConfig(CONFIG_UINT32_MIN_HONOR_KILLS);

    for (auto& pair : m_weeklyScores)
    {
        auto weeklyScore = pair.second;
        HonorStanding standing;
        standing.guid = pair.first;
        standing.cp = weeklyScore.cp;

        if (weeklyScore.hk < minHK || !weeklyScore.account)
        {
            m_inactiveStandingList.push_back(standing);
            continue;
        }

        auto team = sObjectMgr.GetPlayerTeamByGUID(ObjectGuid(HIGHGUID_PLAYER, pair.first));

        if (team == ALLIANCE)
            m_allianceStandingList.push_back(standing);
        else
            m_hordeStandingList.push_back(standing);
    }

    // Make sure all things are sorted
    std::sort(m_allianceStandingList.begin(), m_allianceStandingList.end());
    std::sort(m_hordeStandingList.begin(), m_hordeStandingList.end());

    sLog.outHonor("[MAINTENANCE] Alliance: %u, Horde: %u, Inactive: %u",
        m_allianceStandingList.size(), m_hordeStandingList.size(), m_inactiveStandingList.size());
}

void HonorMaintenancer::DistributeRankPoints(Team team)
{
    HonorStandingList& standingList = GetStandingListByTeam(team);
    if (standingList.empty())
        return;

    HonorScores scores = GenerateScores(standingList);

    uint32 position = 1;

    for (auto& standing : standingList)
    {
        auto itrWS = m_weeklyScores.find(standing.guid);
        if (itrWS == m_weeklyScores.end())
            continue;

        auto& weeklyScore = itrWS->second;

        // Calculate rank points earning
        weeklyScore.earning = CalculateRpEarning(weeklyScore.cp, scores);


        // Calculate rank points with decay
        weeklyScore.newRp = CalculateRpDecay(weeklyScore.earning, weeklyScore);

        // Level restrictions
        weeklyScore.newRp = std::min(MaximumRpAtLevel(weeklyScore.level), weeklyScore.newRp);

        // Set standing to score
        weeklyScore.standing = position;

        ++position;
    }
}

void HonorMaintenancer::InactiveDecayRankPoints()
{
    for (auto& standing : m_inactiveStandingList)
    {
        auto itrWS = m_weeklyScores.find(standing.guid);
        if (itrWS == m_weeklyScores.end())
            continue;

        auto& weeklyScore = itrWS->second;

        weeklyScore.newRp = finiteAlways(CalculateRpDecay(0, weeklyScore));
    }
}

void HonorMaintenancer::SetCityRanks()
{
    CharacterDatabase.Execute("UPDATE `characters` SET `city_protector` = 0");

    std::map<uint8, std::pair<uint32, uint32>> highestStandingInRace =
    {
        {RACE_HUMAN, {0,0}},
        {RACE_ORC, {0,0}},
        {RACE_DWARF, {0,0}},
        {RACE_NIGHTELF, {0,0}},
        {RACE_UNDEAD, {0,0}},
        {RACE_TAUREN, {0,0}},
        {RACE_GNOME, {0,0}},
        {RACE_TROLL, {0,0}},
        {RACE_GOBLIN, {0,0}},
        {RACE_HIGH_ELF, {0,0}},
    };

    for (uint8 i = 1; i < MAX_RACES; ++i)
    {
        QueryResult *result = CharacterDatabase.PQuery("SELECT `guid`, `honorLastWeekHK` FROM characters WHERE `honorLastWeekHK` > 0 and race = '%u' ORDER BY `honorLastWeekHK` DESC LIMIT 1", i);

        if (result)
        {
            do
            {
                Field *fields = result->Fetch();
                uint32 guid = fields[0].GetUInt32();
                uint32 kills = fields[1].GetUInt32();

                highestStandingInRace[i] = std::make_pair(guid, kills);

            } while (result->NextRow());
        }

        delete result;
    }

    for (auto& standing : highestStandingInRace)
    {
        uint32 lowGuid = standing.second.first;

        if (lowGuid > 0)
            CharacterDatabase.PExecute("UPDATE `characters` SET city_protector = 1 WHERE `guid` = '%u'", standing.second.first);
    }
}

void HonorMaintenancer::FlushWeeklyQuests()
{
    //CharacterDatabase.PExecute("DELETE FROM `character_queststatus` WHERE `quest` IN (50322, 50323)");
    CharacterDatabase.PExecute("DELETE FROM `character_queststatus` WHERE `quest` IN (%u, %u)", QUEST_DAILY_MOST_DK, QUEST_DAILY_MOST_HK);
}

void HonorMaintenancer::FlushRankPoints()
{
    // Immediately reset honor standing before flushing
    CharacterDatabase.Execute("UPDATE `characters` SET `honorStanding` = 0 WHERE `honorStanding` > 0");

    for (auto& pair : m_weeklyScores)
    {
        auto weeklyScore = pair.second;

        HonorRankInfo currentRank = HonorMgr::CalculateRank(weeklyScore.newRp);
        HonorRankInfo highestRank;
        HonorMgr::InitRankInfo(highestRank);
        highestRank.rank = weeklyScore.highestRank;
        HonorMgr::CalculateRankInfo(highestRank);

        if (currentRank.visualRank > 0 && (currentRank.visualRank > highestRank.visualRank))
            highestRank = currentRank;

        CharacterDatabase.PExecute("UPDATE `characters` SET `honorHighestRank` = %u, `honorRankPoints` = %.1f, `honorStanding` = %u, "
            "`honorLastWeekHK` = %u, `honorStoredHK` = (`honorStoredHK` + %u), `honorStoredDK` = (`honorStoredDK` + %u), `honorLastWeekCP` = %.1f WHERE `guid` = %u",
            highestRank.rank,
            finiteAlways(weeklyScore.newRp), weeklyScore.standing,
            weeklyScore.hk, weeklyScore.hk, weeklyScore.dk,
            finiteAlways(weeklyScore.cp), pair.first);
    }

    // Not includes weekend day, for correct view in honor tab for group "Yesterday"
    CharacterDatabase.PExecute("DELETE FROM `character_honor_cp` WHERE `date` < %u", GetWeekEndDay());
}

void HonorMaintenancer::DoMaintenance()
{
    if (!m_markerToStart)
        return;

    if (sWorld.getConfig(CONFIG_BOOL_BACKUP_CHARACTER_INVENTORY))
        sObjectMgr.BackupCharacterInventory();

    sLog.outInfo("Beginning character name cleanup...");
    CharacterDatabaseCleaner::FreeInactiveCharacterNames();

    sLog.outHonor("[MAINTENANCE] Honor maintenance starting.");

    {
        std::ofstream honorUpdateFile{ "honorupdate.txt" };
        if (honorUpdateFile)
            honorUpdateFile << "1";
    }

    sLog.outHonor("[MAINTENANCE] Load weekly players scores.");
    LoadWeeklyScores();
    sLog.outHonor("[MAINTENANCE] Load standing lists.");
    LoadStandingLists();
    sLog.outHonor("[MAINTENANCE] Distribute rank points for Alliance.");
    DistributeRankPoints(ALLIANCE);
    sLog.outHonor("[MAINTENANCE] Distribute rank points for Horde.");
    DistributeRankPoints(HORDE);
    sLog.outHonor("[MAINTENANCE] Decay rank points for inactive players.");
    InactiveDecayRankPoints();
    sLog.outHonor("[MAINTENANCE] Flush rank points.");
    FlushRankPoints();
    sLog.outHonor("[MAINTENANCE] Assign city ranks.");
    SetCityRanks();
    sLog.outHonor("[MAINTENANCE] Flush weekly quests.");
    FlushWeeklyQuests();

    CreateCalculationReport();
    sLog.outHonor("[MAINTENANCE] Honor maintenance finished.");

    ToggleMaintenanceMarker();
    SetMaintenanceDays(GetNextMaintenanceDay());
}

void HonorMaintenancer::CreateCalculationReport()
{
    std::string timestamp = Log::GetTimestampStr();
    std::string filename = "HCR_" + timestamp + ".txt";
    std::string path = sWorld.GetHonorPath() + filename;

    std::ofstream ofs;
    ofs.open(path.c_str());
    if (!ofs.is_open())
    {
        sLog.outError("Can't create HCR file!");
        return;
    }

    if (!m_allianceStandingList.empty())
    {
        HonorScores scores = GenerateScores(m_allianceStandingList);

        ofs << "Alliance Honor Scores" << std::endl << std::endl;
        ofs << "Standing size: " << m_allianceStandingList.size() << std::endl << std::endl;

        ofs << std::flush;

        for (auto i = 0; i < 14; ++i)
            ofs << "BRK[" << i << "] = " << scores.BRK[i] << std::endl;

        ofs << std::endl;
        ofs << std::flush;

        for (auto i = 0; i < 15; ++i)
            ofs << "FX[" << i << "] = " << scores.FX[i] << std::endl;

        ofs << std::endl;
        ofs << std::flush;

        for (auto i = 0; i < 15; ++i)
            ofs << "FY[" << i << "] = " << scores.FY[i] << std::endl;

        ofs << std::endl;
        ofs << std::flush;

        for (auto& st : m_allianceStandingList)
        {
            auto itrWS = m_weeklyScores.find(st.guid);
            if (itrWS == m_weeklyScores.end())
                continue;

            auto ws = itrWS->second;

            ofs << "Guid: " << st.guid
                << ", HK: " << ws.hk
                << ", DK: " << ws.dk
                << ", CP: " << ws.cp
                << ", oldRp: " << ws.oldRp
                << ", earning: " << ws.earning
                << ", newRp: " << ws.newRp
                << ", capRp: " << MaximumRpAtLevel(ws.level)
                << ", standing: " << ws.standing << std::endl << std::flush;
        }
    }

    ofs << "--------------------------------------------------" << std::endl << std::endl << std::flush;

    if (!m_hordeStandingList.empty())
    {
        HonorScores scores = GenerateScores(m_hordeStandingList);

        ofs << "Horde Honor Scores" << std::endl << std::endl;
        ofs << "Standing size: " << m_hordeStandingList.size() << std::endl << std::endl;

        ofs << std::flush;

        for (auto i = 0; i < 14; ++i)
            ofs << "BRK[" << i << "] = " << scores.BRK[i] << std::endl;

        ofs << std::endl;
        ofs << std::flush;

        for (auto i = 0; i < 15; ++i)
            ofs << "FX[" << i << "] = " << scores.FX[i] << std::endl;

        ofs << std::endl;
        ofs << std::flush;

        for (auto i = 0; i < 15; ++i)
            ofs << "FY[" << i << "] = " << scores.FY[i] << std::endl;

        ofs << std::endl;
        ofs << std::flush;

        for (auto& st : m_hordeStandingList)
        {
            auto itrWS = m_weeklyScores.find(st.guid);
            if (itrWS == m_weeklyScores.end())
                continue;

            auto ws = itrWS->second;

            ofs << "Guid: " << st.guid
                << ", HK: " << ws.hk
                << ", DK: " << ws.dk
                << ", CP: " << ws.cp
                << ", oldRp: " << ws.oldRp
                << ", earning: " << ws.earning
                << ", newRp: " << ws.newRp
                << ", capRp: " << MaximumRpAtLevel(ws.level)
                << ", standing: " << ws.standing << std::endl << std::flush;
        }
    }

    ofs << "--------------------------------------------------" << std::endl << std::endl << std::flush;

    if (!m_inactiveStandingList.empty())
    {
        ofs << "Inactive players decay" << std::endl << std::endl;
        ofs << "Count: " << m_inactiveStandingList.size() << std::endl << std::endl;
        ofs << std::flush;

        for (auto& st : m_inactiveStandingList)
        {
            auto itrWS = m_weeklyScores.find(st.guid);
            if (itrWS == m_weeklyScores.end())
                continue;

            auto ws = itrWS->second;

            ofs << "Guid: " << st.guid
                << ", HK: " << ws.hk
                << ", DK: " << ws.dk
                << ", CP: " << ws.cp
                << ", oldRp: " << ws.oldRp
                << ", newRp: " << ws.newRp
                << ", capRp: " << MaximumRpAtLevel(ws.level)
                << ", standing: " << ws.standing << std::endl << std::flush;
        }
    }

    ofs.close();
}

HonorScores HonorMaintenancer::GenerateScores(HonorStandingList& standingList)
{
    HonorScores sc;

    sc.BRK[13] = 0.003f;
    sc.BRK[12] = 0.008f;
    sc.BRK[11] = 0.020f;
    sc.BRK[10] = 0.035f;
    sc.BRK[9] = 0.060f;
    sc.BRK[8] = 0.100f;
    sc.BRK[7] = 0.159f;
    sc.BRK[6] = 0.228f;
    sc.BRK[5] = 0.327f;
    sc.BRK[4] = 0.436f;
    sc.BRK[3] = 0.566f;
    sc.BRK[2] = 0.697f;
    sc.BRK[1] = 0.845f;
    sc.BRK[0] = 1.000f;

    // get the WS scores at the top of each break point
    for (float & group : sc.BRK)
        group = floor((group * (standingList.size() > FAKE_PVP_POOL ? standingList.size() : FAKE_PVP_POOL)) + 0.5f); // Turtle WoW Custom: Fake PvP player pool
    // initialize RP array
    // set the low point
    sc.FY[0] = 0;

    // the Y values for each breakpoint are fixed
    sc.FY[1] = 400;
    for (uint8 i = 2; i <= 13; i++)
        sc.FY[i] = (i - 1) * 1000;

    // and finally
    sc.FY[14] = 13000; // ... gets 13000 RP

    // the X values for each breakpoint are found from the CP scores
    // of the players around that point in the WS schcores
    float honor;
    float tempHonor;

    // initialize CP array
    sc.FX[0] = 0;

    bool top = false;
    for (uint8 i = 1; i <= 13; i++)
    {
        honor = 0.0f;
        tempHonor = GetStandingCPByPosition(standingList, sc.BRK[i]);

        if (tempHonor)
        {
            honor += tempHonor;
            tempHonor = GetStandingCPByPosition(standingList, sc.BRK[i] + 1);

            if (tempHonor)
                honor += tempHonor;
        }

        sc.FX[i] = honor ? honor / 2 : 0;

        if (!top && !honor)
        {
            // top scorer
            sc.FX[i] = sc.FX[i - 1] ? standingList.begin()->cp : 0;
            top = true;
        }
    }

    // set the high point if FX full filled before
    // top scorer
    sc.FX[14] = !top ? standingList.begin()->cp : 0;

    return sc;
}

float HonorMaintenancer::CalculateRpEarning(float cp, HonorScores sc)
{
    // search the function for the two points that bound the given CP
    uint8 i = 0;
    while (i < 14 && sc.BRK[i] > 0 && sc.FX[i] <= cp)
        i++;

    // we now have i such that FX[i] > cp >= FX[i-1]
    // so interpolate
    if (sc.FX[i] > cp && cp >= sc.FX[i - 1])
        return (sc.FY[i] - sc.FY[i - 1]) * (cp - sc.FX[i - 1]) / (sc.FX[i] - sc.FX[i - 1]) + sc.FY[i - 1];

    return sc.FY[i];
}

float HonorMaintenancer::CalculateRpDecay(float rpEarning, const WeeklyScore& wk)
{
    //RP per rank needed, starting from rank 2
    static const uint32 RankMinRP[] =
    {
        2000,
        5000,
        10000,
        15000,
        20000,
        25000,
        30000,
        35000,
        40000,
        45000,
        50000,
        55000,
        60000
    };

    float decay = floor((0.2f * wk.oldRp) + 0.5f);
    float delta = rpEarning - decay;

    if (delta < 0)
        delta = delta / 2;

    if (delta < -2500)
        delta = -2500;

    float newRp = wk.oldRp + delta;

    if (wk.highestRank > 1 + NEGATIVE_HONOR_RANK_COUNT)
    {
        // -2 because -1 for 0 based accessing and another -1 for starting at rank 2 because rank 1 only has HK req.
        //And -4 for dishonorable ranks
        float minRpForRank = (float)RankMinRP[wk.highestRank - 2 - NEGATIVE_HONOR_RANK_COUNT];
        if (newRp < minRpForRank)
            newRp = minRpForRank;
    }
    return newRp;
}

float HonorMaintenancer::MaximumRpAtLevel(uint8 level)
{
    if (level <= 29)
        return 6500;
    if (level >= 30 && level <= 35)
        return 7150 + 975 * (level - 30);
    if (level >= 36 && level <= 39)
        return 12025 + 1300 * (level - 35);
    if (level >= 40 && level <= 43)
        return 17225 + 1625 * (level - 39);
    if (level >= 44 && level <= 52)
        return 23725 + 2275 * (level - 43);
    if (level >= 53 && level <= 60)
        return 44200 + 2600 * (level - 52);
    return 65000;
}

void HonorMaintenancer::CheckMaintenanceDay()
{
    if (sWorld.GetGameDay() >= m_nextMaintenanceDay && !m_markerToStart)
    {
        // Restart 15 minutes after honor weekend by server time
        if (sWorld.getConfig(CONFIG_BOOL_AUTO_HONOR_RESTART))
            sWorld.ShutdownServ(900, SHUTDOWN_MASK_RESTART, SHUTDOWN_EXIT_CODE);
        else
            sLog.outString("HonorMaintenancer: Server needs to be restarted to perform honor rank calculations.");

        ToggleMaintenanceMarker();
    }
}

void HonorMaintenancer::ToggleMaintenanceMarker()
{
    m_markerToStart = !m_markerToStart;
    CharacterDatabase.PExecute("INSERT INTO `saved_variables` (`key`, `honorMaintenanceMarker`) VALUES (0, %u) "
        "ON DUPLICATE KEY UPDATE `honorMaintenanceMarker` = %u", m_markerToStart, m_markerToStart);
}

void HonorMaintenancer::SetMaintenanceDays(uint32 last, uint32 next)
{
    m_lastMaintenanceDay = last;

    if (!next)
        m_nextMaintenanceDay = m_lastMaintenanceDay + 7;

    CharacterDatabase.PExecute("INSERT INTO `saved_variables` (`key`, `lastHonorMaintenanceDay`, `nextHonorMaintenanceDay`) VALUES (0, %u, %u) "
        "ON DUPLICATE KEY UPDATE `lastHonorMaintenanceDay` = %u, `nextHonorMaintenanceDay` = %u",
        m_lastMaintenanceDay, m_nextMaintenanceDay, m_lastMaintenanceDay, m_nextMaintenanceDay);
}

void HonorMaintenancer::Initialize()
{
    QueryResult* result = CharacterDatabase.Query("SELECT `lastHonorMaintenanceDay`, `nextHonorMaintenanceDay`, `honorMaintenanceMarker` FROM `saved_variables`");
    if (result)
    {
        Field* fields = result->Fetch();
        m_lastMaintenanceDay = fields[0].GetUInt32();
        m_nextMaintenanceDay = fields[1].GetUInt32();
        m_markerToStart = fields[2].GetBool();
        delete result;
    }

    if (!m_lastMaintenanceDay)
        SetMaintenanceDays(sWorld.GetLastMaintenanceDay());
}

void HonorMgr::ClearHonorData()
{
    m_honorCP.clear();
    m_totalHK = 0;
    m_totalDK = 0;
    m_storedHK = 0;
    m_storedDK = 0;
    m_standing = 0;
    m_lastWeekHK = 0;
    m_rankPoints = 0.0f;
    m_lastWeekCP = 0.0f;
    InitRankInfo(m_rank);
    InitRankInfo(m_highestRank);
}

void HonorMgr::Reset()
{
    if (!m_owner)
        return;

    ClearHonorData();

    // It will delete all honor cp from database imediatly
    CharacterDatabase.PExecute("DELETE FROM `character_honor_cp` WHERE `guid` = %u", m_owner->GetGUIDLow());
    SaveStoredData();

    Update();
}

void HonorMgr::ClearHonorCP()
{
    m_honorCP.clear();
}

void HonorMgr::Save()
{
    if (!m_owner)
        return;

    HonorCPMap tempCP;

    for (auto& honorCP : m_honorCP)
    {

        switch (honorCP.state)
        {
            case STATE_NEW:
                CharacterDatabase.PExecute("INSERT INTO `character_honor_cp` (`guid`, `victimType`, `victim`, `cp`, `date`, `type`) "
                    " VALUES (%u, %u, %u, %.1f, %u, %u)", m_owner->GetGUIDLow(), honorCP.victimType, honorCP.victimId,
                    finiteAlways(honorCP.cp), honorCP.date, honorCP.type);
                honorCP.state = STATE_UNCHANGED;
                tempCP.push_back(honorCP);
                break;
            case STATE_UNCHANGED:
                tempCP.push_back(honorCP);
                break;
            default:
                break;
        }
    }

    m_honorCP.clear();
    m_honorCP = tempCP;
    tempCP.clear();

    // Static data, used for armory
    /*CharacterDatabase.PExecute("DELETE FROM `character_honor_static` WHERE `guid` = %u", m_owner->GetGUIDLow());
    std::ostringstream ss;
    ss << "INSERT INTO `character_honor_static` (`guid`, `hk`, `dk`, `today_hk`, `today_dk`, "
        "`yesterday_kills`, `yesterday_cp`, `thisWeek_kills`, `thisWeek_cp`, `lastWeek_kills`, `lastWeek_cp`) VALUES ("
        << m_owner->GetGUIDLow() << ", "
        << m_owner->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS) << ", "
        << m_owner->GetUInt32Value(PLAYER_FIELD_LIFETIME_DISHONORABLE_KILLS) << ", "
        << m_owner->GetUInt16Value(PLAYER_FIELD_SESSION_KILLS, 0) << ", "
        << m_owner->GetUInt16Value(PLAYER_FIELD_SESSION_KILLS, 1) << ", "
        << m_owner->GetUInt32Value(PLAYER_FIELD_YESTERDAY_KILLS) << ", "
        << m_owner->GetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION) << ", "
        << m_owner->GetUInt32Value(PLAYER_FIELD_THIS_WEEK_KILLS) << ", "
        << m_owner->GetUInt32Value(PLAYER_FIELD_THIS_WEEK_CONTRIBUTION) << ", "
        << m_owner->GetUInt32Value(PLAYER_FIELD_LAST_WEEK_KILLS) << ", "
        << m_owner->GetUInt32Value(PLAYER_FIELD_LAST_WEEK_CONTRIBUTION) << ")";
    CharacterDatabase.Execute(ss.str().c_str());*/
}

void HonorMgr::SaveStoredData()
{
    if (!m_owner)
        return;

    CharacterDatabase.PExecute("UPDATE `characters` SET `honorRankPoints` = %.1f, `honorStanding` = %u, `honorHighestRank` = %u, "
            "`honorLastWeekHK` = %u, `honorLastWeekCP` = %.1f, `honorStoredHK` = %u, `honorStoredDK` = %u WHERE `guid` = %u",
            finiteAlways(m_rankPoints), m_standing, m_highestRank.rank, m_lastWeekHK,
            finiteAlways(m_lastWeekCP), m_storedHK, m_storedDK, m_owner->GetGUIDLow());
}

void HonorMgr::Load(QueryResult* result)
{
    if (result)
    {
        m_honorCP.clear();

        do
        {
            Field* fields = result->Fetch();

            HonorCP honorCP;
            honorCP.victimType = fields[0].GetUInt8();
            honorCP.victimId   = fields[1].GetUInt32();
            honorCP.cp         = fields[2].GetFloat();
            honorCP.date       = fields[3].GetUInt32();
            honorCP.type       = fields[4].GetUInt8();
            honorCP.state      = STATE_UNCHANGED;

            m_honorCP.push_back(honorCP);
        }
        while (result->NextRow());

        // result will be delete in character query holder later
    }
}

bool HonorMgr::Add(float cp, uint8 type, Unit* source)
{
    // Prevent give fake records to db with 0 honor
    if (cp <= 0 || !m_owner)
        return false;

    // If not source, then give yourself
    Unit* realSource = source;
    if (!source)
        source = m_owner;

    HonorCP honorCP;
    honorCP.date = sWorld.GetGameDay();
    honorCP.cp = cp;
    honorCP.victimId = (source->GetTypeId() == TYPEID_PLAYER ? source->GetGUIDLow() : source->GetEntry());
    honorCP.victimType = (source == m_owner ? 0 : source->GetTypeId());
    honorCP.type = type;
    honorCP.state = STATE_NEW;

    float honor = (type == DISHONORABLE) ? -cp : cp;

    // get IP if source is player
    std::string ip;
    if (Player* victim = source->ToPlayer())
        ip = victim->GetSession()->GetRemoteAddress();

    bool plr = source->GetTypeId() == TYPEID_PLAYER;

    if (m_owner->GetMap()->IsBattleGround())
        sLog.outHonor("[BATTLEGROUND]: Player %s (account: %u) got %f honor for type %u, source %s %s (IP: %s)",
            m_owner->GetSession()->GetPlayerName(), m_owner->GetSession()->GetAccountId(), honor, type, plr ? "player" : "unit", source->GetName(), ip.c_str());
    else
        sLog.outHonor("[OPEN WORLD]: Player %s (account: %u) got %f honor for type %u, source %s %s (IP: %s)",
            m_owner->GetSession()->GetPlayerName(), m_owner->GetSession()->GetAccountId(), honor, type, plr ? "player" : "unit", source->GetName(), ip.c_str());

	// DK penalties are subtracted from your RP score immediately and are not included in weekly adjustment.
    if (type == DISHONORABLE)
        m_rankPoints = m_rankPoints - honorCP.cp;

    m_honorCP.push_back(honorCP);

    SendPVPCredit(realSource, honor);

    Update();
    return true;
}

void HonorMgr::Update() {
    if (!m_owner)
        return;

    uint32 todayHK = 0;
    uint32 todayDK = 0;
    uint32 yesterdayKills = 0;
    uint32 thisWeekKills = 0;
    float yesterdayCP = 0.0f;
    float thisWeekCP = 0.0f;

    uint32 today = sWorld.GetGameDay();
    uint32 yesterday = today - 1;
    uint32 thisWeekBegin = sHonorMaintenancer.GetWeekBeginDay();

    m_totalDK = m_storedDK;
    m_totalHK = m_storedHK;

    for (auto &itr : m_honorCP) {
        if (itr.type == HONORABLE) {
            if (itr.date == today)
                ++todayHK;

            if (itr.date == yesterday)
                ++yesterdayKills;

            if (itr.date >= thisWeekBegin) {
                ++thisWeekKills;
                ++m_totalHK;
            }
        }

        if (itr.type != DISHONORABLE) {
            if (itr.date == yesterday)
                yesterdayCP += itr.cp;

            if (itr.date >= thisWeekBegin)
                thisWeekCP += itr.cp;
        }

        if (itr.type == DISHONORABLE) {
            if (itr.date == today)
                ++todayDK;

            ++m_totalDK;
        }
    }

    m_rank = CalculateRank(m_rankPoints, m_totalHK);
    if (m_rank.visualRank > 0 && (m_rank.visualRank > m_highestRank.visualRank))
        SetHighestRank(m_rank);

    // HIGHEST RANK
    m_owner->SetByteValue(PLAYER_FIELD_BYTES, 3, m_highestRank.rank);
    // RANK (Patent)
    if (!m_owner->IsIgnoringTitles()) {
        m_owner->SetByteValue(PLAYER_BYTES_3, 3, m_rank.rank);
    
        uint32 honorBar = uint32(m_rankPoints >= 0.0f ? m_rankPoints : -1 * m_rankPoints);
        honorBar = uint8(((honorBar - m_rank.minRP) / (m_rank.maxRP - m_rank.minRP)) * (m_rank.positive ? 255 : -255));
    
        // PLAYER_FIELD_HONOR_BAR
        m_owner->SetByteValue(PLAYER_FIELD_BYTES2, 0, honorBar);
    }

    // TODAY
    m_owner->SetUInt16Value(PLAYER_FIELD_SESSION_KILLS, 0, todayHK);
    m_owner->SetUInt16Value(PLAYER_FIELD_SESSION_KILLS, 1, todayDK);

    // YESTERDAY
    m_owner->SetUInt32Value(PLAYER_FIELD_YESTERDAY_KILLS, yesterdayKills);
    m_owner->SetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION, uint32(yesterdayCP > 0.0f ? yesterdayCP : 0.0f));

    // THIS WEEK
    m_owner->SetUInt32Value(PLAYER_FIELD_THIS_WEEK_KILLS, thisWeekKills);
    m_owner->SetUInt32Value(PLAYER_FIELD_THIS_WEEK_CONTRIBUTION, uint32(thisWeekCP > 0.0f ? thisWeekCP : 0.0f));

    // LAST WEEK
    m_owner->SetUInt32Value(PLAYER_FIELD_LAST_WEEK_KILLS, m_lastWeekHK);
    m_owner->SetUInt32Value(PLAYER_FIELD_LAST_WEEK_CONTRIBUTION, uint32(m_lastWeekCP > 0.0f ? m_lastWeekCP : 0.0f));
    m_owner->SetUInt32Value(PLAYER_FIELD_LAST_WEEK_RANK, m_standing);

    // LIFE TIME
    m_owner->SetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS, m_totalHK);
    m_owner->SetUInt32Value(PLAYER_FIELD_LIFETIME_DISHONORABLE_KILLS, m_totalDK);
}

void HonorMgr::InitRankInfo(HonorRankInfo &prk)
{
    prk.positive = true;
    prk.rank = 0;
    prk.visualRank = 0;
    prk.maxRP = 2000.00f;
    prk.minRP = 0.00f;
}

uint32 HonorMgr::CalculateTotalKills(Unit* victim) const
{
    if (!victim)
        return 0;

    uint32 totalKills = 0;
    uint32 id = 0;
    uint32 today = sWorld.GetGameDay();
    uint8 victimType = victim->GetTypeId();

    switch (victimType)
    {
        case TYPEID_PLAYER:
            id = ((Player*)victim)->GetGUIDLow();
            break;
        case TYPEID_UNIT:
            id = victim->GetEntry();
            break;
        default:
            return 0;
    }

    for (auto& honorCP : m_honorCP)
        if (honorCP.victimType == victimType && honorCP.victimId == id && honorCP.date == today)
            totalKills++;

    return totalKills;
}

void HonorMgr::CalculateRankInfo(HonorRankInfo& prk)
{
    if (prk.rank != 0)
    {
        int8 rank = prk.positive ? prk.rank - NEGATIVE_HONOR_RANK_COUNT - 1 : prk.rank - NEGATIVE_HONOR_RANK_COUNT;
        prk.maxRP = (rank) * 5000.00f;
        if (prk.maxRP < 0) // in negative rank case
            prk.maxRP *= -1;
        prk.minRP = prk.maxRP > 5000.0f ? prk.maxRP  - 5000.00f : 2000.00f;

        if (prk.rank == 5)
        {
            prk.maxRP = 2000.0f;
            prk.minRP = 0.0f;
        }
        prk.visualRank = prk.rank > NEGATIVE_HONOR_RANK_COUNT ? prk.rank - NEGATIVE_HONOR_RANK_COUNT : prk.rank * -1;
    }
    else
        InitRankInfo(prk);
}

HonorRankInfo HonorMgr::CalculateRank(float rankPoints, uint32 totalHK)
{
    HonorRankInfo prk;
    InitRankInfo(prk);

    // rank none
    if (rankPoints == 0)
        return prk;

    prk.positive = rankPoints > 0;
    if (!prk.positive)
        rankPoints *= -1;

    uint8 rankCount = prk.positive ? POSITIVE_HONOR_RANK_COUNT - 2 : NEGATIVE_HONOR_RANK_COUNT;
    uint8 firstRank = prk.positive ? NEGATIVE_HONOR_RANK_COUNT + 1 : 1;

    if (rankPoints < 2000.00f)
        prk.rank = prk.positive ? firstRank : NEGATIVE_HONOR_RANK_COUNT;
    else
    {
        if (rankPoints > (rankCount - 1) * 5000.00f)
            prk.rank = prk.positive ? HONOR_RANK_COUNT - 1 : firstRank;
        else
        {
            prk.rank = uint32(rankPoints / 5000.00f) + firstRank;
            prk.rank = (prk.positive ? prk.rank  + 1 : NEGATIVE_HONOR_RANK_COUNT - prk.rank);
        }
    }

    CalculateRankInfo(prk);

    return prk;
}

float HonorMgr::DishonorableKillPoints(uint8 level)
{
    float result = 10.0f;
    if (level >= 30 && level <= 35)
        result = result + 1.5f * (level - 29);
    if (level >= 36 && level <= 41)
        result = result + 9 + 2 * (level - 35);
    if (level >= 42 && level <= 50)
        result = result + 21 + 3.2f * (level - 41);
    if (level >= 51)
        result = result + 50 + 4 * (level - 50);
    if (result > 100)
        return 100.0f;
    else
        return result;
}

float HonorMgr::HonorableKillPoints(Player* killer, Player* victim, uint32 groupSize)
{
    if (!killer || !victim || !groupSize)
        return 0.0;

    uint32 totalKills = killer->GetHonorMgr().CalculateTotalKills(victim);
    uint32 victimRank = victim->GetHonorMgr().GetRank().visualRank;
    uint8 killerLevel = killer->GetLevel();
    uint8 victimLevel = victim->GetLevel();

    return MaNGOS::Honor::GetHonorGain(killerLevel, victimLevel, victimRank, totalKills, groupSize, killer->InGurubashiArena(false));
}

void HonorMgr::SendPVPCredit(Unit* victim, float honor)
{
    if (!m_owner)
        return;

    WorldPacket data(SMSG_PVP_CREDIT, 4 + 8 + 4);
    data << int32(honor);

    if (!victim)
    {
        data << int64(0);
        data << int32(0);
    }
    else
    {
        data << victim->GetObjectGuid();

        constexpr int minimumRank = (HONOR_RANK_COUNT - POSITIVE_HONOR_RANK_COUNT) + 1;

        if (victim->IsCreature())
        {
            if (((Creature*)victim)->IsRacialLeader())
                data << int32(19);
            else
                data << int32(minimumRank);
        }
        else if (victim->IsPlayer())
        {
            // Never display just "HK:" without rank name.
            // When killing a player with no rank,
            // we need to send first rank instead.
            // https://youtu.be/hef06Cs6Q34?t=191
            // New classic client does this on its own.
            int32 rank = ((Player*)victim)->GetHonorMgr().GetRank().rank;
            if (!rank)
                rank = minimumRank;

            data << int32(rank);
        }
    }

    m_owner->SendDirectMessage(&data);
}
