#include "nparticle.h"
#include "../system/inivals.h"
#include "../skeleton.h"
#include "../ade.h"
#include "../particle.h"
#include "../base.h"
#include "../reforge_fx.h"
#include "../reforge_trails.h"

namespace World
{

#ifdef UA_VR_ENABLED
float ParticleViewScale = 1.0f;
#endif

ParticleSystem::ParticleSystem()
{
}
    
void ParticleSystem::AddParticle(NC_STACK_particle *base, const vec3d& pos, const vec3d& vec, int32_t age)
{
    if (!_disableAdd && (int32_t)_particles.size() < System::IniConf::ParticlesLimit.Get<int32_t>())
        _particles.emplace_back( base, pos, vec, age, ++_seedCounter );
}

// Simulation step and render queueing are separate (VR): the
// update must run exactly once per game tick, while queueing only reads the
// particle state and the current view. UpdateRender() keeps the original
// one-call behaviour (same erase order, same RNG draws, same queue order).
void ParticleSystem::UpdateRender(area_arg_65 *rndrParams, int32_t delta)
{
    //Prevent spawn new particles from particle render
    _disableAdd = true;

    Update(delta);
    QueueRender(rndrParams);

    _disableAdd = false;
}

void ParticleSystem::Update(int32_t delta)
{
    float fsec = (float)delta * 0.001;
    
    for(auto it = _particles.begin(); it != _particles.end();)
    {
        Frak &f = *it;
        if (!f.pParticleGen)
        {
            it = _particles.erase(it);
            continue;
        }
        
        f.Age += delta;
        
        if (f.Age >= f.pParticleGen->_lifeTime)
        {
            it = _particles.erase(it);
            continue;
        }
        
        f.Vec += f.pParticleGen->_accelStart + f.pParticleGen->_accelDelta * f.Age;
        f.Pos += f.Vec * fsec + NC_STACK_particle::RandVec() * f.pParticleGen->_noisePower;
        
        it++;
    }
}

void ParticleSystem::QueueRender(area_arg_65 *rndrParams)
{
    for(Frak &f : _particles)
    {
        float scl = f.pParticleGen->_scaleStart + f.pParticleGen->_scaleDelta * f.Age;
        Render(&f, scl, rndrParams);
    }
}


void ParticleSystem::Render(Frak *p, float scale, area_arg_65 *rndrParams)
{
    TF::TForm3D *view = rndrParams->ViewTForm;
#ifdef UA_VR_ENABLED
    scale *= ParticleViewScale; // VR diorama: billboards shrink with the miniature
#endif

    // Transformed position, but without of projection
    vec3d pos = view->CalcSclRot.Transform(p->Pos - view->CalcPos);
    
    if (pos.z < GFX::Engine.GetProjectionNear() || pos.z > GFX::Engine.GetProjectionFar())
        return;
    
    // Calculate projection position and W parameter for NDC
    const mat4x4f &Proj = GFX::Engine.GetProjectionMatrix();
    
    vec3d projPos = Proj.Transform(pos);
    float w = Proj.CalcW(pos);
    
    /* Radius of 2.0 side square = sqrt(1.0^2 + 1.0^2) = 1.414
     * And applied W
     */
    float radius = scale * 1.42 / w;

    vec2f scrPos(projPos.x / w, projPos.y / w);
    
    // If transformed point length more than 
    if ( scrPos.length() > 1.42 + radius )
        return;

    if ( p->pParticleGen->_lifePerAde )
    {
        size_t id = p->Age / p->pParticleGen->_lifePerAde;

        if (id == p->pParticleGen->_meshCache.size()) // fix little overlap
            id = p->pParticleGen->_meshCache.size() - 1;

        // gfx TRAILS level >= 2: a 1998 trail our smoke replaces is not drawn (0/1 = original)
        if ( ReforgeTrails::Level() >= 2 && ReforgeTrails::HideParticle(p->pParticleGen) )
            return;

        // Reforge fx (UA_FX pack): our own sprite for a mapped stage; same
        // particle, position, size and age - only the picture changes.
        if ( id < p->pParticleGen->_meshCache.size() && ReforgeFx::On() &&
             ReforgeFx::RenderParticle(p->pParticleGen, id, p->Age, p->Seed, p->Pos, scale, rndrParams) )
            return;

        if ( id < p->pParticleGen->_meshCache.size() )
        {
            GFX::TMesh &mesh = p->pParticleGen->_meshCache.at(id);

            GFX::TRenderNode& rend = GFX::Engine.AllocRenderNode();
            rend = GFX::TRenderNode( GFX::TRenderNode::TYPE_PARTICLE );

            rend.Distance = pos.length();
            rend.Flags = mesh.Mat.Flags | rndrParams->flags;
            rend.Color = mesh.Mat.Color;

            /*if (newsky)
            {
                if ( distance >= transDist ||
                     skel132.tform.Transform(msh.BoundBox.Min).XZ().length() >= transDist ||
                     skel132.tform.Transform(msh.BoundBox.Max).XZ().length() >= transDist )
                    rend.Mat.Flags |= GFX::RFLAGS_FALLOFF;
            }*/

            if ((mesh.Mat.Flags & GFX::RFLAGS_DYNAMIC_TEXTURE) && mesh.Mat.TexSource)
            {
                mesh.Mat.TexSource->SetTime(rndrParams->timeStamp, rndrParams->frameTime);
                uint32_t frameid = mesh.Mat.TexSource->GetCurrentFrameID();

                if (frameid < mesh.CoordsCache.size())
                {
                    rend.Tex = mesh.CoordsCache.at(frameid).Tex;
                    rend.coordsID = frameid;                    
                }
            }
            else
                rend.Tex = mesh.Mat.Tex;

            rend.Mesh = &mesh;        
            
            rend.TForm = mat4x4(scale, 0.0, 0.0, pos.x,
                                0.0, scale, 0.0, pos.y,
                                0.0,   0.0, 1.0, pos.z,
                                0.0,   0.0, 0.0, 1.0);
            rend.TimeStamp = rndrParams->timeStamp;
            rend.FrameTime = rndrParams->frameTime;
            rend.FogStart = rndrParams->fadeStart;
            rend.FogLength = rndrParams->fadeLength;
            rend.ParticleSize = scale;

            GFX::GFXEngine::Instance.QueueRenderMesh(&rend);
        }
    }
}

void ParticleSystem::Clear()
{
    _particles.clear();
}

}