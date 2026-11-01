#ifndef AITUNING_H
#define AITUNING_H

// Data-driven commander tuning ("ai-tuning.txt") + the classic/influence
// commander toggle.
//
// The 1998 AI's magic numbers — desire weights, the desire-delay formula, the
// build reserve and price surcharge, aggression-threshold semantics, morale
// hysteresis and the unit-brain cadences — were welded into ypabact.cpp and
// yparobo.cpp. This module lifts them into one struct of tunables that can be
// overridden by a plain text file read from the engine CWD at startup (the CWD
// is the game-data directory, same as every other data file the engine reads).
//
// Contract:
//  - No file present  -> every value below keeps its compiled-in default AND
//    the commander runs the CLASSIC path: behavior is bit-identical to a build
//    without this module. Float-context constants are stored as double with
//    the exact original literals so the arithmetic stays identical.
//  - File present     -> values are overridden per key; the commander defaults
//    to the INFLUENCE path unless the file says "commander = classic".
//  - UA_AI_TUNING=<path> overrides the file location (testing aid, so smokes
//    never have to write into the player's game-data directory).
//  - UA_AI_DEBUG=1 -> dump every effective value at load and log commander
//    decisions to stderr, "[UA_AI] ..." (same pattern as UA_JOY_DEBUG).
//
// Nothing here is ever persisted: saves (SAVE/<profile>/user.txt) are
// untouched, so save-format stays byte-compatible with the 0.5 build.

namespace AITune
{

enum ECommanderMode
{
    COMMANDER_CLASSIC   = 0, // verbatim 1998 target selection
    COMMANDER_INFLUENCE = 1, // influence-map-modulated target selection
};

struct Values
{
    // ---- Commander mode (see ECommanderMode). Absent file => CLASSIC. ----
    int CommanderMode = COMMANDER_CLASSIC;

    // ---- Commander economy (yparobo.cpp AllocForce) ----
    double ReserveFraction = 0.2;  // :2382 — untouchable fraction of robo energy_max
    double PriceSurcharge  = 0.05; // :2375 — +5% of total fielded energy per unit price
    double DelayScale      = 1.0;  // multiplies timeline*(100-budget)/100 (all 8 desires)

    // ---- Force sizing (yparobo.cpp buildConquer/buildDefense/buildRobo/buildReconnoitre) ----
    int ForceConquerFloor = 15000;  // :2598
    int ForceConquerMin   = 20000;  // :2667
    int ForceDefenseFloor = 15000;  // :2709
    int ForceDefenseCap   = 40000;  // :2758
    int ForceRoboFloor    = 120000; // :2624 :2725 :2842
    int ForceReconEnergy  = 10;     // :2907

    // ---- Aggression handed to commander-built squads ----
    int AggrConquer     = 60; // buildConquer :2677
    int AggrConquerPowerStation = 49; // buildConquer :2675 — capture quietly, just under engage threshold
    int AggrDefense     = 60; // buildDefense :2765, buildRobo :2849
    int AggrRecon       = 25; // buildReconnoitre :2908/:2921
    int AggrNewUnit     = 60; // AllocForce :2506 — freshly beamed vehicle default

    // ---- Aggression threshold semantics (ypabact.cpp; UI steps of 25 untouched) ----
    int AggrKeepPrimary        = 25;  // TargetAssess :6253/:6364 — keep fighting the ordered target
    int AggrEngageSecondary    = 50;  // AI_layer2 :1166, TargetAssess :6247 — pick up threats on its own
    int AggrOpportunistCapture = 75;  // AI_layer2 :1185 (==), TargetAssess :6356 — grab sectors passed over
    int AggrBerserk            = 100; // TargetAssess :6263/:6381 (>=), GetFightMotivation :5810 (==)

    // ---- Morale hysteresis (ypabact.cpp GetFightMotivation) ----
    double MoraleFightRatio = 0.2; // :5841 — own/enemy energy ratio to fight
    double MoraleRallyRatio = 0.5; // :5837 — ratio to rally once fleeing

    // ---- Unit-brain cadences, ms (ypabact.cpp) ----
    int CadenceLayer1Ms     = 250; // :965  — lifecycle/order relay tick
    int CadenceLayer2Ms     = 250; // :1091 — target-acquisition tick
    int CadenceThreatScanMs = 500; // :1131 — sector threat scan

    // ---- Desire scorer weights (yparobo.cpp yparobo_func70__sub6__sub*) ----
    // Conquer (sub4 :3780)
    double DesireConquerBase        = 1000.0; // score = base/distance
    int    DesireConquerSelf        = 1500;   // own-cell distance<=0 case
    int    DesireConquerPurposeFlat = 20;     // special-purpose sector flat score
    int    DesireConquerGateBonus   = 40;     // beam gate
    int    DesireConquerPowerBonus  = 50;     // power station
    int    DesireConquerAdjFriendly = 3;      // per adjacent own sector
    int    DesireConquerEnemyRobo   = 100;    // per enemy host station sighted
    int    DesireConquerEnemyUnit   = 5;      // per enemy unit sighted
    // Defense (sub5 :3986 + sub_4F4C6C :3968)
    int    DesireDefenseStationThreat  = 500;   // enemy within 2 sectors of the station
    double DesireDefenseBase           = 100.0; // squad score = base/distance (2*base at 0)
    double DesireDefenseTerritoryBonus = 100.0; // squad inside our territory
    // Radar (sub0 :3616, gate :4417)
    double DesireRadarUnseenNeighbor = 10.0; // per fogged neighbor sector
    int    DesireRadarCoverageDiv    = 64;   // build while unseen/div > own radars
    // Recon (sub1 :3661)
    double DesireReconUnseenNeighbor = 5.0;  // per fogged neighbor sector
    // Power (sub3 :3726)
    double DesirePowerAdjacentFactor = 0.7;  // penalty if a power station is adjacent
    double DesirePowerUnitPenalty    = 5.0;  // per unit loitering in the build sector
    // Safety/flak (sub2 :3684)
    double DesireSafetyAdjBuilding   = 5.0;  // per adjacent own special-purpose sector

    // ---- Influence map (only active with CommanderMode == INFLUENCE) ----
    int    InfluenceTickMs        = 500;   // rebuild cadence, the strategic tick
    double InfluenceSpread        = 0.4;   // neighbor bleed in the blur pass
    double InfluenceThreatDecay   = 0.95;  // threat memory per tick (~14 s half-life)
    double InfluenceThreatNorm    = 10000.0; // energy scale: threat/(threat+K) -> [0,1)
    double InfluenceConquerWeight = 40.0;  // conquer-score shift per unit of net advantage [-1..1]
    double InfluenceDefenseWeight = 100.0; // defense-score bonus per unit of normalized threat [0..1)
};

// The one live instance every consumer reads.
extern Values g;

// UA_AI_DEBUG=1 in the environment (cached after first call).
bool Debug();

// Read UA_AI_TUNING (or ./ai-tuning.txt) once at startup. Safe to call before
// any world exists; logs the effective values to stderr when Debug().
void Load();

// "[UA_AI] " printf to stderr; no-op unless Debug().
void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// The desire-delay formula, one home instead of eight copy-pastes:
// classic delay = timeScale * (100 - budget) / 100, then scaled.
inline int DesireDelay(int timeScale, int budget)
{
    return (int)( g.DelayScale * ( timeScale * (100 - budget) / 100 ) );
}

}

#endif
