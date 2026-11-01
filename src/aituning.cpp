// Loader for ai-tuning.txt + the UA_AI_DEBUG log channel.
// See aituning.h for the contract.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <fstream>

#include "aituning.h"

namespace AITune
{

Values g;

bool Debug()
{
    static int cached = -1;
    if ( cached < 0 )
    {
        const char *v = getenv("UA_AI_DEBUG");
        cached = ( v && v[0] && strcmp(v, "0") != 0 ) ? 1 : 0;
    }
    return cached != 0;
}

void Log(const char *fmt, ...)
{
    if ( !Debug() )
        return;

    fputs("[UA_AI] ", stderr);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static std::string Trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if ( b == std::string::npos )
        return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// One table drives parsing, the debug dump and the docs. Every entry is either
// an int or a double slot in g.
struct TKey
{
    const char *name;
    int    *pi;
    double *pd;
};

static const TKey Keys[] =
{
    // commander mode is handled separately ("commander = influence|classic")
    { "commander.reserve_fraction",     NULL, &g.ReserveFraction },
    { "commander.price_surcharge",      NULL, &g.PriceSurcharge },
    { "commander.delay_scale",          NULL, &g.DelayScale },

    { "force.conquer_floor",            &g.ForceConquerFloor, NULL },
    { "force.conquer_min",              &g.ForceConquerMin, NULL },
    { "force.defense_floor",            &g.ForceDefenseFloor, NULL },
    { "force.defense_cap",              &g.ForceDefenseCap, NULL },
    { "force.robo_floor",               &g.ForceRoboFloor, NULL },
    { "force.recon_energy",             &g.ForceReconEnergy, NULL },

    { "aggr.conquer",                   &g.AggrConquer, NULL },
    { "aggr.conquer_powerstation",      &g.AggrConquerPowerStation, NULL },
    { "aggr.defense",                   &g.AggrDefense, NULL },
    { "aggr.recon",                     &g.AggrRecon, NULL },
    { "aggr.new_unit",                  &g.AggrNewUnit, NULL },

    { "aggr.keep_primary",              &g.AggrKeepPrimary, NULL },
    { "aggr.engage_secondary",          &g.AggrEngageSecondary, NULL },
    { "aggr.opportunist_capture",       &g.AggrOpportunistCapture, NULL },
    { "aggr.berserk",                   &g.AggrBerserk, NULL },

    { "morale.fight_ratio",             NULL, &g.MoraleFightRatio },
    { "morale.rally_ratio",             NULL, &g.MoraleRallyRatio },

    { "cadence.layer1_ms",              &g.CadenceLayer1Ms, NULL },
    { "cadence.layer2_ms",              &g.CadenceLayer2Ms, NULL },
    { "cadence.threat_scan_ms",         &g.CadenceThreatScanMs, NULL },

    { "desire.conquer.base",            NULL, &g.DesireConquerBase },
    { "desire.conquer.self",            &g.DesireConquerSelf, NULL },
    { "desire.conquer.purpose_flat",    &g.DesireConquerPurposeFlat, NULL },
    { "desire.conquer.gate_bonus",      &g.DesireConquerGateBonus, NULL },
    { "desire.conquer.power_bonus",     &g.DesireConquerPowerBonus, NULL },
    { "desire.conquer.adj_friendly",    &g.DesireConquerAdjFriendly, NULL },
    { "desire.conquer.enemy_robo",      &g.DesireConquerEnemyRobo, NULL },
    { "desire.conquer.enemy_unit",      &g.DesireConquerEnemyUnit, NULL },

    { "desire.defense.station_threat",  &g.DesireDefenseStationThreat, NULL },
    { "desire.defense.base",            NULL, &g.DesireDefenseBase },
    { "desire.defense.territory_bonus", NULL, &g.DesireDefenseTerritoryBonus },

    { "desire.radar.unseen_neighbor",   NULL, &g.DesireRadarUnseenNeighbor },
    { "desire.radar.coverage_div",      &g.DesireRadarCoverageDiv, NULL },
    { "desire.recon.unseen_neighbor",   NULL, &g.DesireReconUnseenNeighbor },
    { "desire.power.adjacent_factor",   NULL, &g.DesirePowerAdjacentFactor },
    { "desire.power.unit_penalty",      NULL, &g.DesirePowerUnitPenalty },
    { "desire.safety.adj_building",     NULL, &g.DesireSafetyAdjBuilding },

    { "influence.tick_ms",              &g.InfluenceTickMs, NULL },
    { "influence.spread",               NULL, &g.InfluenceSpread },
    { "influence.threat_decay",         NULL, &g.InfluenceThreatDecay },
    { "influence.threat_norm",          NULL, &g.InfluenceThreatNorm },
    { "influence.conquer_weight",       NULL, &g.InfluenceConquerWeight },
    { "influence.defense_weight",       NULL, &g.InfluenceDefenseWeight },
};

static void DumpEffective(const char *origin)
{
    Log("tuning source: %s\n", origin);
    Log("tune commander = %s\n",
        g.CommanderMode == COMMANDER_INFLUENCE ? "influence" : "classic");

    for ( const TKey &k : Keys )
    {
        if ( k.pi )
            Log("tune %s = %d\n", k.name, *k.pi);
        else
            Log("tune %s = %.6g\n", k.name, *k.pd);
    }
}

void Load()
{
    const char *envPath = getenv("UA_AI_TUNING");
    const char *path = ( envPath && envPath[0] ) ? envPath : "ai-tuning.txt";

    std::ifstream f(path);

    if ( !f.is_open() )
    {
        // No tuning file: compiled-in defaults, classic commander,
        // bit-identical to a build without the tuning code.
        if ( envPath && envPath[0] )
            fprintf(stderr, "[UA_AI] UA_AI_TUNING points to unreadable '%s', using built-in defaults\n", path);

        if ( Debug() )
            DumpEffective("built-in defaults (no ai-tuning.txt)");
        return;
    }

    // The file's presence opts into the modern commander unless it says
    // otherwise ("commander = classic").
    g.CommanderMode = COMMANDER_INFLUENCE;

    int lineNo = 0;
    std::string line;
    while ( std::getline(f, line) )
    {
        lineNo++;

        size_t hash = line.find('#');
        if ( hash != std::string::npos )
            line.erase(hash);

        line = Trim(line);
        if ( line.empty() )
            continue;

        size_t eq = line.find('=');
        if ( eq == std::string::npos )
        {
            fprintf(stderr, "[UA_AI] %s:%d: ignored line without '=': %s\n", path, lineNo, line.c_str());
            continue;
        }

        std::string key = Trim( line.substr(0, eq) );
        std::string val = Trim( line.substr(eq + 1) );

        if ( key.empty() || val.empty() )
        {
            fprintf(stderr, "[UA_AI] %s:%d: ignored malformed line\n", path, lineNo);
            continue;
        }

        if ( key == "commander" )
        {
            if ( val == "influence" )
                g.CommanderMode = COMMANDER_INFLUENCE;
            else if ( val == "classic" )
                g.CommanderMode = COMMANDER_CLASSIC;
            else
                fprintf(stderr, "[UA_AI] %s:%d: commander must be 'influence' or 'classic', got '%s'\n",
                        path, lineNo, val.c_str());
            continue;
        }

        bool matched = false;
        for ( const TKey &k : Keys )
        {
            if ( key == k.name )
            {
                char *end = NULL;
                if ( k.pi )
                {
                    long v = strtol(val.c_str(), &end, 10);
                    if ( end && *end == '\0' )
                        *k.pi = (int)v;
                    else
                        fprintf(stderr, "[UA_AI] %s:%d: '%s' wants an integer, got '%s'\n",
                                path, lineNo, key.c_str(), val.c_str());
                }
                else
                {
                    double v = strtod(val.c_str(), &end);
                    if ( end && *end == '\0' )
                        *k.pd = v;
                    else
                        fprintf(stderr, "[UA_AI] %s:%d: '%s' wants a number, got '%s'\n",
                                path, lineNo, key.c_str(), val.c_str());
                }
                matched = true;
                break;
            }
        }

        if ( !matched )
            fprintf(stderr, "[UA_AI] %s:%d: unknown key '%s' ignored\n", path, lineNo, key.c_str());
    }

    if ( Debug() )
        DumpEffective(path);
}

}
