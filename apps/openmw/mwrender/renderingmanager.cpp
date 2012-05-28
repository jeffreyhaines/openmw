#include "renderingmanager.hpp"

#include <assert.h>

#include "OgreRoot.h"
#include "OgreRenderWindow.h"
#include "OgreSceneManager.h"
#include "OgreViewport.h"
#include "OgreCamera.h"
#include "OgreTextureManager.h"

#include "../mwworld/world.hpp" // these includes can be removed once the static-hack is gone
#include "../mwworld/ptr.hpp"
#include "../mwworld/player.hpp"
#include "../mwbase/environment.hpp"
#include <components/esm/loadstat.hpp>
#include <components/settings/settings.hpp>

#include "shadows.hpp"
#include "shaderhelper.hpp"
#include "localmap.hpp"
#include "water.hpp"
#include "compositors.hpp"

#include "../mwgui/window_manager.hpp" // FIXME
#include "../mwinput/inputmanager.hpp" // FIXME

using namespace MWRender;
using namespace Ogre;

namespace MWRender {

RenderingManager::RenderingManager (OEngine::Render::OgreRenderer& _rend, const boost::filesystem::path& resDir, OEngine::Physic::PhysicEngine* engine)
    :mRendering(_rend), mObjects(mRendering), mActors(mRendering), mAmbientMode(0), mSunEnabled(0)
{
    mRendering.createScene("PlayerCam", Settings::Manager::getFloat("field of view", "General"), 5);
    mRendering.setWindowEventListener(this);

    mCompositors = new Compositors(mRendering.getViewport());

    mWater = 0;

    //The fog type must be set before any terrain objects are created as if the
    //fog type is set to FOG_NONE then the initially created terrain won't have any fog
    configureFog(1, ColourValue(1,1,1));

    // Set default mipmap level (NB some APIs ignore this)
    TextureManager::getSingleton().setDefaultNumMipmaps(Settings::Manager::getInt("num mipmaps", "General"));

    // Set default texture filtering options
    TextureFilterOptions tfo;
    std::string filter = Settings::Manager::getString("texture filtering", "General");
    if (filter == "anisotropic") tfo = TFO_ANISOTROPIC;
    else if (filter == "trilinear") tfo = TFO_TRILINEAR;
    else if (filter == "bilinear") tfo = TFO_BILINEAR;
    else if (filter == "none") tfo = TFO_NONE;

    MaterialManager::getSingleton().setDefaultTextureFiltering(tfo);
    MaterialManager::getSingleton().setDefaultAnisotropy( (filter == "anisotropic") ? Settings::Manager::getInt("anisotropy", "General") : 1 );

    // Load resources
    ResourceGroupManager::getSingleton().initialiseAllResourceGroups();

    // Due to the huge world size of MW, we'll want camera-relative rendering.
    // This prevents precision artifacts when moving very far from the origin.
    mRendering.getScene()->setCameraRelativeRendering(true);

    // disable unsupported effects
    const RenderSystemCapabilities* caps = Root::getSingleton().getRenderSystem()->getCapabilities();
    if (caps->getNumMultiRenderTargets() < 2 || !Settings::Manager::getBool("shaders", "Objects"))
        Settings::Manager::setBool("shader", "Water", false);
    if ( !(caps->isShaderProfileSupported("fp40") || caps->isShaderProfileSupported("ps_4_0"))
        || !Settings::Manager::getBool("shaders", "Objects"))
        Settings::Manager::setBool("enabled", "Shadows", false);

    // note that the order is important here
    if (useMRT())
    {
        mCompositors->addCompositor("gbuffer", 0);
        mCompositors->setCompositorEnabled("gbuffer", true);
        mCompositors->addCompositor("Underwater", 1);
        mCompositors->addCompositor("gbufferFinalizer", 2);
        mCompositors->setCompositorEnabled("gbufferFinalizer", true);
    }
    else
    {
        mCompositors->addCompositor("UnderwaterNoMRT", 0);
    }

    // Turn the entire scene (represented by the 'root' node) -90
    // degrees around the x axis. This makes Z go upwards, and Y go into
    // the screen (when x is to the right.) This is the orientation that
    // Morrowind uses, and it automagically makes everything work as it
    // should.
    SceneNode *rt = mRendering.getScene()->getRootSceneNode();
    mMwRoot = rt->createChildSceneNode();
    mMwRoot->pitch(Degree(-90));
    mObjects.setMwRoot(mMwRoot);
    mActors.setMwRoot(mMwRoot);

    Ogre::SceneNode *playerNode = mMwRoot->createChildSceneNode ("player");
    playerNode->pitch(Degree(90));
    Ogre::SceneNode *cameraYawNode = playerNode->createChildSceneNode();
    Ogre::SceneNode *cameraPitchNode = cameraYawNode->createChildSceneNode();
    cameraPitchNode->attachObject(mRendering.getCamera());

    mShadows = new Shadows(&mRendering);
    mShaderHelper = new ShaderHelper(this);

    mTerrainManager = new TerrainManager(mRendering.getScene(), this);

    //mSkyManager = 0;
    mSkyManager = new SkyManager(mMwRoot, mRendering.getCamera());

    mOcclusionQuery = new OcclusionQuery(&mRendering, mSkyManager->getSunNode());

    mPlayer = new MWRender::Player (mRendering.getCamera(), playerNode);
    mSun = 0;

    mDebugging = new Debugging(mMwRoot, engine);
    mLocalMap = new MWRender::LocalMap(&mRendering, this);

    setMenuTransparency(Settings::Manager::getFloat("menu transparency", "GUI"));
}

RenderingManager::~RenderingManager ()
{
    delete mPlayer;
    delete mSkyManager;
    delete mDebugging;
    delete mTerrainManager;
    delete mLocalMap;
    delete mOcclusionQuery;
    delete mCompositors;
}

MWRender::SkyManager* RenderingManager::getSkyManager()
{
    return mSkyManager;
}

MWRender::Objects& RenderingManager::getObjects(){
    return mObjects;
}
MWRender::Actors& RenderingManager::getActors(){
    return mActors;
}

MWRender::Player& RenderingManager::getPlayer(){
    return (*mPlayer);
}

OEngine::Render::Fader* RenderingManager::getFader()
{
    return mRendering.getFader();
}

void RenderingManager::removeCell (MWWorld::Ptr::CellStore *store)
{
    mObjects.removeCell(store);
    mActors.removeCell(store);
    mDebugging->cellRemoved(store);
    if (store->cell->isExterior())
      mTerrainManager->cellRemoved(store);
}

void RenderingManager::removeWater ()
{
    if(mWater){
        mWater->setActive(false);
    }
}

void RenderingManager::toggleWater()
{
    if (mWater)
        mWater->toggle();
}

void RenderingManager::cellAdded (MWWorld::Ptr::CellStore *store)
{
    mObjects.buildStaticGeometry (*store);
    mDebugging->cellAdded(store);
    if (store->cell->isExterior())
      mTerrainManager->cellAdded(store);
    waterAdded(store);
}

void RenderingManager::addObject (const MWWorld::Ptr& ptr){
    const MWWorld::Class& class_ =
            MWWorld::Class::get (ptr);
    class_.insertObjectRendering(ptr, *this);

}
void RenderingManager::removeObject (const MWWorld::Ptr& ptr)
{
    if (!mObjects.deleteObject (ptr))
    {
        /// \todo delete non-object MW-references
    }
     if (!mActors.deleteObject (ptr))
    {
        /// \todo delete non-object MW-references
    }
}

void RenderingManager::moveObject (const MWWorld::Ptr& ptr, const Ogre::Vector3& position)
{
    /// \todo move this to the rendering-subsystems
    mRendering.getScene()->getSceneNode (ptr.getRefData().getHandle())->
            setPosition (position);
}

void RenderingManager::scaleObject (const MWWorld::Ptr& ptr, const Ogre::Vector3& scale){

}
void RenderingManager::rotateObject (const MWWorld::Ptr& ptr, const::Ogre::Quaternion& orientation){

}
void RenderingManager::moveObjectToCell (const MWWorld::Ptr& ptr, const Ogre::Vector3& position, MWWorld::Ptr::CellStore *store){

}

void RenderingManager::update (float duration){

    mActors.update (duration);
    mObjects.update (duration);

    mOcclusionQuery->update(duration);

    mSkyManager->update(duration);

    mSkyManager->setGlare(mOcclusionQuery->getSunVisibility());

    mRendering.update(duration);

    mLocalMap->updatePlayer( mRendering.getCamera()->getRealPosition(), mRendering.getCamera()->getRealOrientation() );

    checkUnderwater();

    mWater->update();
}
void RenderingManager::waterAdded (MWWorld::Ptr::CellStore *store){
    if(store->cell->data.flags & store->cell->HasWater
        || ((!(store->cell->data.flags & ESM::Cell::Interior))
            && !MWBase::Environment::get().getWorld()->getStore().lands.search(store->cell->data.gridX,store->cell->data.gridY) )) // always use water, if the cell does not have land.
    {
        if(mWater == 0)
            mWater = new MWRender::Water(mRendering.getCamera(), this, store->cell);
        else
            mWater->changeCell(store->cell);
        mWater->setActive(true);
    }
    else
        removeWater();
}

void RenderingManager::setWaterHeight(const float height)
{
    if (mWater)
        mWater->setHeight(height);
}

void RenderingManager::skyEnable ()
{
    if(mSkyManager)
    mSkyManager->enable();

    mOcclusionQuery->setSunNode(mSkyManager->getSunNode());
}

void RenderingManager::skyDisable ()
{
    if(mSkyManager)
        mSkyManager->disable();
}

void RenderingManager::skySetHour (double hour)
{
    if(mSkyManager)
        mSkyManager->setHour(hour);
}


void RenderingManager::skySetDate (int day, int month)
{
    if(mSkyManager)
        mSkyManager->setDate(day, month);
}

int RenderingManager::skyGetMasserPhase() const
{

    return mSkyManager->getMasserPhase();
}

int RenderingManager::skyGetSecundaPhase() const
{
    return mSkyManager->getSecundaPhase();
}

void RenderingManager::skySetMoonColour (bool red){
    if(mSkyManager)
        mSkyManager->setMoonColour(red);
}

bool RenderingManager::toggleRenderMode(int mode)
{
    if (mode == MWWorld::World::Render_CollisionDebug || mode == MWWorld::World::Render_Pathgrid)
        return mDebugging->toggleRenderMode(mode);
    else if (mode == MWWorld::World::Render_Wireframe)
    {
        if (mRendering.getCamera()->getPolygonMode() == PM_SOLID)
        {
            mCompositors->setEnabled(false);

            mRendering.getCamera()->setPolygonMode(PM_WIREFRAME);
            return true;
        }
        else
        {
            mCompositors->setEnabled(true);

            mRendering.getCamera()->setPolygonMode(PM_SOLID);
            return false;
        }
    }
    else //if (mode == MWWorld::World::Render_Compositors)
    {
        return mCompositors->toggle();
    }
}

void RenderingManager::configureFog(ESMS::CellStore<MWWorld::RefData> &mCell)
{
    Ogre::ColourValue color;
    color.setAsABGR (mCell.cell->ambi.fog);

    configureFog(mCell.cell->ambi.fogDensity, color);
}

void RenderingManager::configureFog(const float density, const Ogre::ColourValue& colour)
{
    float max = Settings::Manager::getFloat("max viewing distance", "Viewing distance");

    float low = max / (density) * Settings::Manager::getFloat("fog start factor", "Viewing distance");
    float high = max / (density) * Settings::Manager::getFloat("fog end factor", "Viewing distance");

    mRendering.getScene()->setFog (FOG_LINEAR, colour, 0, low, high);

    mRendering.getCamera()->setFarClipDistance ( max / density );
    mRendering.getViewport()->setBackgroundColour (colour);

    CompositorInstance* inst = CompositorManager::getSingleton().getCompositorChain(mRendering.getViewport())->getCompositor("gbuffer");
    if (inst != 0)
        inst->getCompositor()->getTechnique(0)->getTargetPass(0)->getPass(0)->setClearColour(colour);
    if (mWater)
        mWater->setViewportBackground(colour);
}


void RenderingManager::setAmbientMode()
{
  switch (mAmbientMode)
  {
    case 0:

      setAmbientColour(mAmbientColor);
      break;

    case 1:

      setAmbientColour(0.7f*mAmbientColor + 0.3f*ColourValue(1,1,1));
      break;

    case 2:

      setAmbientColour(ColourValue(1,1,1));
      break;
  }
}

void RenderingManager::configureAmbient(ESMS::CellStore<MWWorld::RefData> &mCell)
{
    mAmbientColor.setAsABGR (mCell.cell->ambi.ambient);
    setAmbientMode();

    // Create a "sun" that shines light downwards. It doesn't look
    // completely right, but leave it for now.
    if(!mSun)
    {
        mSun = mRendering.getScene()->createLight();
    }
    Ogre::ColourValue colour;
    colour.setAsABGR (mCell.cell->ambi.sunlight);
    mSun->setDiffuseColour (colour);
    mSun->setType(Ogre::Light::LT_DIRECTIONAL);
    mSun->setDirection(0,-1,0);
}
// Switch through lighting modes.

void RenderingManager::toggleLight()
{
    if (mAmbientMode==2)
        mAmbientMode = 0;
    else
        ++mAmbientMode;

    switch (mAmbientMode)
    {
        case 0: std::cout << "Setting lights to normal\n"; break;
        case 1: std::cout << "Turning the lights up\n"; break;
        case 2: std::cout << "Turning the lights to full\n"; break;
    }

    setAmbientMode();
}
void RenderingManager::checkUnderwater()
{
    if(mWater)
    {
         mWater->checkUnderwater( mRendering.getCamera()->getRealPosition().y );
    }
}

void RenderingManager::playAnimationGroup (const MWWorld::Ptr& ptr, const std::string& groupName,
     int mode, int number)
{
    mActors.playAnimationGroup(ptr, groupName, mode, number);
}

void RenderingManager::skipAnimation (const MWWorld::Ptr& ptr)
{
    mActors.skipAnimation(ptr);
}

void RenderingManager::setSunColour(const Ogre::ColourValue& colour)
{
    if (!mSunEnabled) return;
    mSun->setDiffuseColour(colour);
    mSun->setSpecularColour(colour);
    mTerrainManager->setDiffuse(colour);
}

void RenderingManager::setAmbientColour(const Ogre::ColourValue& colour)
{
    mRendering.getScene()->setAmbientLight(colour);
    mTerrainManager->setAmbient(colour);
}

void RenderingManager::sunEnable()
{
    // Don't disable the light, as the shaders assume the first light to be directional.
    //if (mSun) mSun->setVisible(true);
    mSunEnabled = true;
}

void RenderingManager::sunDisable()
{
    // Don't disable the light, as the shaders assume the first light to be directional.
    //if (mSun) mSun->setVisible(false);
    mSunEnabled = false;
    if (mSun)
    {
        mSun->setDiffuseColour(ColourValue(0,0,0));
        mSun->setSpecularColour(ColourValue(0,0,0));
    }
}

void RenderingManager::setSunDirection(const Ogre::Vector3& direction)
{
    // direction * -1 (because 'direction' is camera to sun vector and not sun to camera),
    // then convert from MW to ogre coordinates (swap y,z and make y negative)
    if (mSun) mSun->setDirection(Vector3(-direction.x, -direction.z, direction.y));

    mSkyManager->setSunDirection(direction);
}

void RenderingManager::setGlare(bool glare)
{
    mSkyManager->setGlare(glare);
}

void RenderingManager::requestMap(MWWorld::Ptr::CellStore* cell)
{
    if (!(cell->cell->data.flags & ESM::Cell::Interior))
        mLocalMap->requestMap(cell);
    else
        mLocalMap->requestMap(cell, mObjects.getDimensions(cell));
}

void RenderingManager::preCellChange(MWWorld::Ptr::CellStore* cell)
{
    mLocalMap->saveFogOfWar(cell);
}

void RenderingManager::disableLights()
{
    mObjects.disableLights();
    sunDisable();
}

void RenderingManager::enableLights()
{
    mObjects.enableLights();
    sunEnable();
}

const bool RenderingManager::useMRT()
{
    return Settings::Manager::getBool("shader", "Water");
}

Shadows* RenderingManager::getShadows()
{
    return mShadows;
}

void RenderingManager::switchToInterior()
{
    mRendering.getScene()->setCameraRelativeRendering(false);
}

void RenderingManager::switchToExterior()
{
    mRendering.getScene()->setCameraRelativeRendering(true);
}

Ogre::Vector4 RenderingManager::boundingBoxToScreen(Ogre::AxisAlignedBox bounds)
{
    Ogre::Matrix4 mat = mRendering.getCamera()->getViewMatrix();

    const Ogre::Vector3* corners = bounds.getAllCorners();

    float min_x = 1.0f, max_x = 0.0f, min_y = 1.0f, max_y = 0.0f;

    // expand the screen-space bounding-box so that it completely encloses
    // the object's AABB
    for (int i=0; i<8; i++)
    {
        Ogre::Vector3 corner = corners[i];

        // multiply the AABB corner vertex by the view matrix to
        // get a camera-space vertex
        corner = mat * corner;

        // make 2D relative/normalized coords from the view-space vertex
        // by dividing out the Z (depth) factor -- this is an approximation
        float x = corner.x / corner.z + 0.5;
        float y = corner.y / corner.z + 0.5;

        if (x < min_x)
        min_x = x;

        if (x > max_x)
        max_x = x;

        if (y < min_y)
        min_y = y;

        if (y > max_y)
        max_y = y;
    }

    return Vector4(min_x, min_y, max_x, max_y);
}

Compositors* RenderingManager::getCompositors()
{
    return mCompositors;
}

void RenderingManager::processChangedSettings(const Settings::CategorySettingVector& settings)
{
    bool changeRes = false;
    for (Settings::CategorySettingVector::const_iterator it=settings.begin();
            it != settings.end(); ++it)
    {
        if (it->second == "menu transparency" && it->first == "GUI")
        {
            setMenuTransparency(Settings::Manager::getFloat("menu transparency", "GUI"));
        }
        else if (it->second == "max viewing distance" && it->first == "Viewing distance")
        {
            if (!MWBase::Environment::get().getWorld()->isCellExterior() && !MWBase::Environment::get().getWorld()->isCellQuasiExterior())
                configureFog(*MWBase::Environment::get().getWorld()->getPlayer().getPlayer().getCell());
        }
        else if (it->first == "Video" && (
                it->second == "resolution x"
                || it->second == "resolution y"
                || it->second == "fullscreen"))
            changeRes = true;
    }

    if (changeRes)
    {
        unsigned int x = Settings::Manager::getInt("resolution x", "Video");
        unsigned int y = Settings::Manager::getInt("resolution y", "Video");

        if (x != mRendering.getWindow()->getWidth() || y != mRendering.getWindow()->getHeight())
        {
            mRendering.getWindow()->resize(x, y);
        }
        mRendering.getWindow()->setFullscreen(Settings::Manager::getBool("fullscreen", "Video"), x, y);
    }
}

void RenderingManager::setMenuTransparency(float val)
{
    Ogre::TexturePtr tex = Ogre::TextureManager::getSingleton().getByName("transparent.png");
    std::vector<Ogre::uint32> buffer;
    buffer.resize(1);
    buffer[0] = (int(255*val) << 24);
    memcpy(tex->getBuffer()->lock(Ogre::HardwareBuffer::HBL_DISCARD), &buffer[0], 1*4);
    tex->getBuffer()->unlock();
}

void RenderingManager::windowResized(Ogre::RenderWindow* rw)
{
    Settings::Manager::setInt("resolution x", "Video", rw->getWidth());
    Settings::Manager::setInt("resolution y", "Video", rw->getHeight());


    mRendering.adjustViewport();
    mCompositors->recreate();
    mWater->assignTextures();

    const Settings::CategorySettingVector& changed = Settings::Manager::apply();
    MWBase::Environment::get().getInputManager()->processChangedSettings(changed); //FIXME
    MWBase::Environment::get().getWindowManager()->processChangedSettings(changed); // FIXME
}

void RenderingManager::windowClosed(Ogre::RenderWindow* rw)
{
}

} // namespace
