#include "mayaMVG/core/MVGProject.hpp"
#include "mayaMVG/core/MVGCamera.hpp"
#include "mayaMVG/core/MVGPointCloud.hpp"
#include "mayaMVG/core/MVGMesh.hpp"
#include "mayaMVG/core/MVGLog.hpp"
#include "mayaMVG/maya/context/MVGContextCmd.hpp"
#include "mayaMVG/maya/MVGMayaUtil.hpp"
#include <maya/MFnTransform.h>
#include <maya/MItDependencyNodes.h>
#include <maya/MDagModifier.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MSelectionList.h>
#include <maya/MPlug.h>
#include <maya/MQuaternion.h>
#include <maya/MEulerRotation.h>
#include <maya/MMatrix.h>
#include <maya/MItSelectionList.h>
#include <maya/MItMeshPolygon.h>

namespace
{ // empty namespace

void lockNode(MObject obj)
{
    if(obj.apiType() != MFn::kTransform)
        return;
    MStatus status;
    MFnDagNode fn(obj, &status);
    CHECK_RETURN(status)
    fn.findPlug("translateX").setLocked(true);
    fn.findPlug("translateY").setLocked(true);
    fn.findPlug("translateZ").setLocked(true);
    fn.findPlug("rotateX").setLocked(true);
    fn.findPlug("rotateY").setLocked(true);
    fn.findPlug("rotateZ").setLocked(true);

    for(int i = 0; i < fn.childCount(); ++i)
        lockNode(fn.child(i));
}

void unlockNode(MObject obj)
{
    if(obj.apiType() != MFn::kTransform)
        return;
    MFnDagNode fn(obj);
    fn.findPlug("translateX").setLocked(false);
    fn.findPlug("translateY").setLocked(false);
    fn.findPlug("translateZ").setLocked(false);
    fn.findPlug("rotateX").setLocked(false);
    fn.findPlug("rotateY").setLocked(false);
    fn.findPlug("rotateZ").setLocked(false);

    for(int i = 0; i < fn.childCount(); ++i)
        unlockNode(fn.child(i));
}

} // empty namespace

namespace mayaMVG
{

std::string MVGProject::_CAMERAS_GROUP = "mvgCameras";
std::string MVGProject::_CLOUD_GROUP = "mvgCloud";
std::string MVGProject::_CLOUD = "mvgPointCloud";
std::string MVGProject::_MESH = "mvgMesh";
std::string MVGProject::_PROJECT = "mvgRoot";
std::string MVGProject::_LOCATOR = "mvgLocator";
MString MVGProject::_MVG_PROJECTPATH = "mvgProjectPath";

// Image cache
// List of camera by name or dagpath according to uniqueness
std::list<std::string> MVGProject::_cachedImagePlanes;
std::map<std::string, std::string> MVGProject::_lastLoadedCameraByView;

MVGProject::MVGProject(const std::string& name)
    : MVGNodeWrapper(name)
{
}

MVGProject::MVGProject(const MDagPath& dagPath)
    : MVGNodeWrapper(dagPath)
{
}

MVGProject::~MVGProject()
{
}

// virtual
bool MVGProject::isValid() const
{
    if(!_dagpath.isValid() || (_dagpath.apiType() != MFn::kTransform))
        return false;
    MFnTransform fn(_dagpath);
    MStatus status;
    fn.findPlug(_MVG_PROJECTPATH, false, &status);
    if(!status)
        return false;
    return true;
}

// static
std::vector<MVGProject> MVGProject::list()
{
    std::vector<MVGProject> list;
    MDagPath path;
    MItDependencyNodes it(MFn::kTransform);
    for(; !it.isDone(); it.next())
    {
        MFnDependencyNode fn(it.thisNode());
        MDagPath::getAPathTo(fn.object(), path);
        MVGProject project(path);
        if(project.isValid())
            list.push_back(project);
    }
    return list;
}

// static
MVGPointCloud MVGProject::getPointCloud()
{
    MDagPath pointCloudGroup;
    getPointcloudGroupPath(pointCloudGroup);
    MDagPath pointCloudDagPath;
    MDagPath::getAPathTo(pointCloudGroup.child(0), pointCloudDagPath);
    pointCloudDagPath.extendToShape();

    return MVGPointCloud(pointCloudDagPath);
}

// static
void MVGProject::getRootPath(MDagPath& dagpath)
{
    MStatus status;
    status = MVGMayaUtil::getDagPathByName(_PROJECT.c_str(), dagpath);
    CHECK(status)
}

// static
void MVGProject::getCameraGroupPath(MDagPath& dagpath)
{
    MDagPath rootPath;
    getRootPath(rootPath);

    // TODO : check name ?
    MObject cameraParent = rootPath.child(0);
    MDagPath::getAPathTo(cameraParent, dagpath);
}

// static
void MVGProject::getPointcloudGroupPath(MDagPath& dagpath)
{
    MDagPath rootPath;
    getRootPath(rootPath);

    // TODO : check name ?
    MObject cloudParent = rootPath.child(1);
    MDagPath::getAPathTo(cloudParent, dagpath);
}

/**
 * Apply transformation on scene :
 * Get locator an apply and set it as origin of scene.
 *
 */
bool MVGProject::applySceneTransformation() const
{
    MStatus status;

    // Check for root node
    if(!isValid())
        return false;

    // Retieve old transformation (in mvgRoot)
    MString rootName(_PROJECT.c_str());
    MObject rootObject;
    MVGMayaUtil::getObjectByName(rootName, rootObject);
    MFnTransform rootTransform(rootObject);
    // L1(-1))
    MTransformationMatrix rootTransformation = rootTransform.transformation();

    // Retrieve locator transformation
    const MString locatorName(_LOCATOR.c_str());
    MObject locatorObject;
    status = MVGMayaUtil::getObjectByName(locatorName, locatorObject);
    if(!status)
    {
        LOG_ERROR("Can't find \"" << locatorName << "\" locator.")
        return false;
    }
    MFnTransform locatorTransform(locatorObject);
    // L2
    MTransformationMatrix locatorTransformation = locatorTransform.transformation();

    // Compute L12(-1) - inverse transfer matrix from old system to new system
    MMatrix transformationMatrix =
        rootTransformation.asMatrixInverse() * locatorTransformation.asMatrixInverse();
    MTransformationMatrix transformation(transformationMatrix);

    // Set transformation to root
    MMatrix newRootTranformationMatrix = rootTransformation.asMatrix() * transformation.asMatrix();
    MTransformationMatrix newRootTranformation(newRootTranformationMatrix);
    rootTransform.set(newRootTranformation);

    // Set transformation to mesh
    std::vector<MVGMesh> activeMeshes = MVGMesh::listActiveMeshes();
    for(std::vector<MVGMesh>::iterator it = activeMeshes.begin(); it != activeMeshes.end(); ++it)
    {
        MDagPath path = it->getDagPath();

        // Retrieve transform
        path.pop();
        MObject transformObj = path.node();
        unlockNode(transformObj);
        MFnTransform transform(path, &status);
        CHECK(status)

        // Apply transformation
        // X(L1))
        MTransformationMatrix meshTransformation = transform.transformation();
        MMatrix newMeshTransformationMatrix = meshTransformation.asMatrix() * transformationMatrix;
        MTransformationMatrix newMeshTransformation(newMeshTransformationMatrix);
        status = transform.set(newMeshTransformation);
        if(!status)
            LOG_ERROR(status.errorString())
        CHECK(status)
        MString cmd;
        cmd.format("makeIdentity -apply true \"^1s\"", path.fullPathName());
        status = MGlobal::executeCommand(cmd);
        CHECK(status)
        lockNode(transformObj);
    }

    return true;
}

void MVGProject::clear()
{
    MDagPath camerasDagPath;
    getCameraGroupPath(camerasDagPath);
    for(int i = camerasDagPath.childCount(); i > 0; --i)
    {
        MObject child = camerasDagPath.child(i - 1);
        MGlobal::deleteNode(child);
    }
    MDagPath pointCloudDagPath;
    getPointcloudGroupPath(pointCloudDagPath);
    for(int i = pointCloudDagPath.childCount(); i > 0; --i)
    {
        MObject child = pointCloudDagPath.child(i - 1);
        MGlobal::deleteNode(child);
    }
}

const std::string MVGProject::getProjectDirectory() const
{
    MString directory;
    // Retrive it by its name
    MDagPath path;
    MVGMayaUtil::getDagPathByName(_PROJECT.c_str(), path);
    MVGMayaUtil::getStringAttribute(path.node(), _MVG_PROJECTPATH, directory);
    return (directory.length() > 0) ? directory.asChar() : "";
}

void MVGProject::setProjectDirectory(const std::string& directory) const
{
    // Retrive it by its name
    MDagPath path;
    MVGMayaUtil::getDagPathByName(_PROJECT.c_str(), path);
    MVGMayaUtil::setStringAttribute(path.node(), _MVG_PROJECTPATH, directory.c_str());
}

void MVGProject::selectCameras(const std::vector<std::string>& cameraNames) const
{
    MSelectionList list;
    for(std::vector<std::string>::const_iterator it = cameraNames.begin(); it != cameraNames.end();
        ++it)
    {
        MVGCamera camera(*it);
        list.add(camera.getDagPath());
    }
    MGlobal::setActiveSelectionList(list);
}

void MVGProject::selectMeshes(const std::vector<std::string>& meshes) const
{
    MSelectionList list;
    for(std::vector<std::string>::const_iterator it = meshes.begin(); it != meshes.end(); ++it)
    {
        MVGMesh mesh(*it);
        list.add(mesh.getDagPath());
    }
    MGlobal::setActiveSelectionList(list);
}

void MVGProject::unlockProject() const
{
    // Camera group
    MDagPath cameraGroupPath;
    getCameraGroupPath(cameraGroupPath);
    MObject cameraGroup = cameraGroupPath.node();

    unlockNode(cameraGroup);

    // Cloud group
    MDagPath pointcloudGroupPath;
    getPointcloudGroupPath(pointcloudGroupPath);
    MObject cloudGroup = pointcloudGroupPath.node();
    unlockNode(cloudGroup);
}

void MVGProject::lockProject() const
{
    // Camera group
    MDagPath cameraGroupPath;
    getCameraGroupPath(cameraGroupPath);
    MObject cameraGroup = cameraGroupPath.node();
    lockNode(cameraGroup);

    // Cloud group
    MDagPath pointcloudGroupPath;
    getPointcloudGroupPath(pointcloudGroupPath);
    MObject cloudGroup = pointcloudGroupPath.node();
    lockNode(cloudGroup);
}

void MVGProject::pushImageInCache(const std::string& cameraName)
{
    if(cameraName.empty())
        return;

    std::list<std::string>::iterator camera =
        std::find(_cachedImagePlanes.begin(), _cachedImagePlanes.end(), cameraName);
    if(camera != _cachedImagePlanes.end()) // Camera is already in the list
        return;

    if(_cachedImagePlanes.size() == IMAGE_CACHE_SIZE)
    {
        std::string frontCamera = _cachedImagePlanes.front();
        MVGCamera camera(frontCamera);
        camera.unloadImagePlane();
        _cachedImagePlanes.pop_front();
    }
    _cachedImagePlanes.push_back(cameraName);
}

const std::string MVGProject::getLastLoadedCameraInView(const std::string& viewName) const
{
    std::map<std::string, std::string>::const_iterator findIt =
        _lastLoadedCameraByView.find(viewName);
    if(findIt == _lastLoadedCameraByView.end())
        return "";
    return findIt->second;
}

void MVGProject::setLastLoadedCameraInView(const std::string& viewName,
                                           const std::string& cameraName)
{
    _lastLoadedCameraByView[viewName] = cameraName;
}

/**
 * Update the image cache :
 *     - If current camera is in cache : remove it
 *     - Push last current camera in cache
 * @param newCameraName current camera
 * @param oldCameraName last current camera that we need to push in cache
 */
void MVGProject::updateImageCache(const std::string& newCameraName,
                                  const std::string& oldCameraName)
{
    // If new camera is in cache remove from cacheList
    std::list<std::string>::iterator cameraIt =
        std::find(_cachedImagePlanes.begin(), _cachedImagePlanes.end(), newCameraName);
    if(cameraIt != _cachedImagePlanes.end())
        _cachedImagePlanes.remove(newCameraName);

    if(oldCameraName != newCameraName)
        pushImageInCache(oldCameraName);
}

/**
 *  Unload all images in camera (except current cameras) and clear cache
 */
void MVGProject::clearImageCache()
{
    _cachedImagePlanes.clear();
}

/**
 * Create a command to load the current image plane corresponding to the camera in the panel.
 * Push the command to the idle queue
 * @param panelName view in which the image plane will be updated
 */
void MVGProject::pushLoadCurrentImagePlaneCommand(const std::string& panelName) const
{
    MStatus status;
    MString cmd;
    // Warning: this command return the NAME of the object if unique.
    // Else, it return the dagpath
    cmd.format("MVGImagePlaneCmd -panel \"^1s\" -load ", panelName.c_str());
    status = MGlobal::executeCommandOnIdle(cmd);
    CHECK_RETURN(status)
}

} // namespace
