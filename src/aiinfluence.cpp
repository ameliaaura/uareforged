// Influence-map rebuild. See aiinfluence.h.

#include "log.h"
#include "yw.h"
#include "aiinfluence.h"
#include "aituning.h"

namespace AI
{

void InfluenceMap::Update(NC_STACK_ypaworld *world, int owner, int clock)
{
    if ( _ever && (clock - _lastClock) < AITune::g.InfluenceTickMs )
        return;

    _lastClock = clock;
    _ever = true;

    Common::PlaneVector<cellArea> &sectors = world->Sectors();

    Common::Point mapSize = world->GetMapSize();

    int w = mapSize.x;
    int h = mapSize.y;

    if ( w <= 0 || h <= 0 )
        return;

    if ( w != _w || h != _h )
    {
        _w = w;
        _h = h;
        _friendRaw.assign(w * h, 0.0f);
        _enemyRaw.assign(w * h, 0.0f);
        _friendly.assign(w * h, 0.0f);
        _enemy.assign(w * h, 0.0f);
        _threat.assign(w * h, 0.0f);
    }

    std::fill(_friendRaw.begin(), _friendRaw.end(), 0.0f);
    std::fill(_enemyRaw.begin(), _enemyRaw.end(), 0.0f);

    // Raw power per sector. Fog-honest: enemy units only count in sectors the
    // owner's faction can see — the same IsCanSee() rule the 1998 scorers obey.
    for ( int i = 0; i < w * h; i++ )
    {
        cellArea &cell = sectors.At((size_t)i);

        if ( cell.IsBorder() )
            continue;

        bool visible = cell.IsCanSee(owner);

        for ( NC_STACK_ypabact* &bct : cell.unitsList )
        {
            if ( bct->_bact_type == BACT_TYPES_MISSLE || bct->_status == BACT_STATUS_DEAD )
                continue;

            if ( bct->_owner == owner )
                _friendRaw[i] += (float)bct->_energy;
            else if ( bct->_owner && visible )
                _enemyRaw[i] += (float)bct->_energy;
        }
    }

    // Threat: decaying memory of sighted enemy power.
    float decay = (float)AITune::g.InfluenceThreatDecay;
    for ( int i = 0; i < w * h; i++ )
        _threat[i] = _threat[i] * decay + _enemyRaw[i];

    // One 8-neighbor blur pass so influence bleeds a ring outward.
    float spread = (float)AITune::g.InfluenceSpread;
    for ( int y = 0; y < h; y++ )
    {
        for ( int x = 0; x < w; x++ )
        {
            float fn = 0.0f;
            float en = 0.0f;
            int cnt = 0;

            for ( int dy = -1; dy <= 1; dy++ )
            {
                for ( int dx = -1; dx <= 1; dx++ )
                {
                    if ( !dx && !dy )
                        continue;

                    int nx = x + dx;
                    int ny = y + dy;

                    if ( nx < 0 || ny < 0 || nx >= w || ny >= h )
                        continue;

                    fn += _friendRaw[nx + ny * w];
                    en += _enemyRaw[nx + ny * w];
                    cnt++;
                }
            }

            int i = x + y * w;

            if ( cnt )
            {
                _friendly[i] = _friendRaw[i] + spread * (fn / cnt);
                _enemy[i]    = _enemyRaw[i]  + spread * (en / cnt);
            }
            else
            {
                _friendly[i] = _friendRaw[i];
                _enemy[i]    = _enemyRaw[i];
            }
        }
    }
}

float InfluenceMap::Advantage(const Common::Point &c) const
{
    if ( !Ready() || !Inside(c) )
        return 0.0f;

    float f = _friendly[c.x + c.y * _w];
    float e = _enemy[c.x + c.y * _w];

    return (f - e) / (f + e + 1.0f);
}

float InfluenceMap::Threat(const Common::Point &c) const
{
    if ( !Ready() || !Inside(c) )
        return 0.0f;

    float t = _threat[c.x + c.y * _w];

    return t / (t + (float)AITune::g.InfluenceThreatNorm);
}

}
