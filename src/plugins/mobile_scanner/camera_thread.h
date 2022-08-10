#ifndef CAMERA_THREAD_H_
#define CAMERA_THREAD_H_

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum CameraThreadResult
    {
        CAMERA_THREAD_SUCCESS,
        CAMERA_THREAD_FAILURE,
    } CameraThreadResult;

    typedef struct CameraThreadState CameraThreadState;
    CameraThreadState *CameraThreadState_new();
    CameraThreadResult CameraThreadState_init(CameraThreadState *self, int device_id);
    void *camera_thread_main(void *arg);
    void CameraThreadState_clean(CameraThreadState *self);
    void CameraThreadState_delete(CameraThreadState *self);

    double CameraThreadState_getHeight(CameraThreadState *self);
    double CameraThreadState_getWidth(CameraThreadState *self);
#ifdef __cplusplus
}
#endif

#endif // CAMERA_THREAD_H_
