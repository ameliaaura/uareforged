#ifndef AIINFLUENCE_H
#define AIINFLUENCE_H

// Sector-grid influence map for the commander AI.
//
// Three layers over the world's sector grid, rebuilt on the 500 ms strategic
// tick (AITune::g.InfluenceTickMs) by each non-player host station:
//   friendly — own units' energy, blurred one ring outward
//   enemy    — visible enemy units' energy (fog-honest: only sectors the
//              owner's radar can see contribute), blurred one ring outward
//   threat   — decaying memory of where enemy power has been sighted
//
// The map only MODULATES target choice inside the commander's existing desire
// scorers (conquer + defense); it never touches budgets, prices or the .ldf
// difficulty inputs, so campaign balance stays anchored. Nothing here is
// serialized. Rebuild cost is O(sectors + units) every half second — a few
// microseconds, no per-frame work at all.

#include <vector>
#include "common/common.h"

class NC_STACK_ypaworld;

namespace AI
{

class InfluenceMap
{
public:
    // Rebuild if the tick interval has elapsed. clock is the caller's local
    // AI clock (NC_STACK_ypabact::_clock), same time base the unit brains use.
    void Update(NC_STACK_ypaworld *world, int owner, int clock);

    bool Ready() const { return _w > 0; }

    // Net local advantage in [-1, 1]: (friendly - enemy) / (friendly + enemy + 1).
    // Positive where our influence dominates, negative in enemy strongholds.
    float Advantage(const Common::Point &c) const;

    // Accumulated threat mapped to [0, 1): threat / (threat + InfluenceThreatNorm).
    float Threat(const Common::Point &c) const;

private:
    bool Inside(const Common::Point &c) const
    {
        return c.x >= 0 && c.y >= 0 && c.x < _w && c.y < _h;
    }

    int _w = 0;
    int _h = 0;
    int _lastClock = 0;
    bool _ever = false;

    std::vector<float> _friendRaw;
    std::vector<float> _enemyRaw;
    std::vector<float> _friendly;
    std::vector<float> _enemy;
    std::vector<float> _threat;
};

}

#endif
