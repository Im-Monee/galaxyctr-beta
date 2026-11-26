#include "Game/Scene/Scene.hpp"

// 3DS: the native renderer forgets the models of the scenes before (platform/3ds/gx/native.cpp).
#ifdef __3DS__
void rvlNativeSceneStart();
#endif

Scene::Scene(const char* pName) : NerveExecutor(pName) {
    mListExecutor = nullptr;
    _C = 0;
    mSceneObjHolder = nullptr;
#ifdef __3DS__
    // A new scene: no model of the scenes before is drawn again, and their memory is used anew.
    rvlNativeSceneStart();
#endif
}

Scene::~Scene() {
    if (mSceneObjHolder != nullptr) {
        delete mSceneObjHolder;
    }

    if (mListExecutor != nullptr) {
        delete mListExecutor;
    }
}

void Scene::init() {
}

void Scene::start() {
}

void Scene::update() {
}

void Scene::draw() const {
}

void Scene::calcAnim() {
}

void Scene::initNameObjListExecutor() {
    SceneNameObjListExecutor* exec = new SceneNameObjListExecutor();
    exec->init();
    mListExecutor = exec;
}

void Scene::initSceneObjHolder() {
    mSceneObjHolder = new SceneObjHolder();
}