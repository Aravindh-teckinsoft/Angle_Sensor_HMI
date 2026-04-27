#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cstdlib> 

#pragma comment(lib, "user32.lib")

static GstElement* pipeline = NULL;
static GstElement* appsink_elem = NULL;
static GMainLoop* main_loop = NULL;
static HANDLE         loop_thread = NULL;

static unsigned char* frame_buffer = NULL;
static int            frame_width = 0;
static int            frame_height = 0;
static size_t         buffer_size = 0;

CRITICAL_SECTION cs;

static void LogMsg(const char* msg)
{
    // Write to VS Output via OutputDebugStringA
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");

    // ALSO write to a log file — always visible
    FILE* f = fopen("D:\\Jetson\\gst_player_log.txt", "a");
    if (f)
    {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

// ================= GLIB MAIN LOOP THREAD =================
// FIX: Without this, g_signal_connect callbacks never fire.
//      GStreamer needs a GLib main loop running on a thread.
static DWORD WINAPI GLibLoopThread(LPVOID)
{
    main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(main_loop);
    return 0;
}

// ================= FRAME CALLBACK =================
static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer)
{
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;

    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buf = gst_sample_get_buffer(sample);

    if (!caps) { gst_sample_unref(sample); return GST_FLOW_ERROR; }

    GstStructure* s = gst_caps_get_structure(caps, 0);
    int w = 0, h = 0;
    gst_structure_get_int(s, "width", &w);
    gst_structure_get_int(s, "height", &h);

    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_READ))
    {
        EnterCriticalSection(&cs);

        if (buffer_size != map.size)
        {
            free(frame_buffer);
            frame_buffer = (unsigned char*)malloc(map.size);
            buffer_size = map.size;
        }

        if (frame_buffer)
        {
            memcpy(frame_buffer, map.data, map.size);
            frame_width = w;
            frame_height = h;
        }

        LeaveCriticalSection(&cs);
        gst_buffer_unmap(buf, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// ================= START STREAM =================
extern "C" __declspec(dllexport)
void StartStream()
{
    InitializeCriticalSection(&cs);
    _putenv("GST_PLUGIN_PATH=C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64\\lib\\gstreamer-1.0");
    //_putenv("PATH=C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64\\bin\");

    gst_init(NULL, NULL);

    // Log what GStreamer actually found after init
    LogMsg("gst_init done");
    // Verify udpsrc is available before building pipeline
    GstElementFactory* f = gst_element_factory_find("udpsrc");
    if (!f)
    {
        LogMsg("ERROR: udpsrc plugin not found — check GST_PLUGIN_PATH");
        return;
    }
    gst_object_unref(f);
    LogMsg("udpsrc plugin found OK");

    // Start GLib main loop on background thread FIRST
    loop_thread = CreateThread(NULL, 0, GLibLoopThread, NULL, 0, NULL);

    // FIX: No hardcoded resolution — let videoconvert negotiate
    //      with whatever the Jetson sends (1920x1200 or any size)
    const char* pipeline_str =
        "udpsrc port=5000 "
        "caps=\"application/x-rtp,media=video,encoding-name=H264,payload=96\" ! "
        "rtph264depay ! avdec_h264 ! "
        "videoconvert ! video/x-raw,format=BGR ! "
        "appsink name=sink emit-signals=true sync=false "
        "max-buffers=1 drop=true";

    GError* err = NULL;
    pipeline = gst_parse_launch(pipeline_str, &err);

    if (!pipeline || err)
    {
        LogMsg(err ? err->message : "Pipeline failed\n");
        if (err) g_error_free(err);
        return;
    }

    appsink_elem = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!appsink_elem)
    {
        LogMsg("Appsink not found\n");
        return;
    }

    // FIX: Set callbacks via GstAppSinkCallbacks — more reliable
    //      than g_signal_connect for cross-thread DLL use
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(
        (GstAppSink*)appsink_elem, &callbacks, NULL, NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    LogMsg("Pipeline started\n");
}

// ================= GET FRAME =================
// FIX: C# P/Invoke passes IntPtr* (pointer to IntPtr).
//      Match exactly: unsigned char** on C++ side.
extern "C" __declspec(dllexport)
bool GetFrame(unsigned char** data, int* width, int* height)
{
    EnterCriticalSection(&cs);

    if (!frame_buffer || frame_width == 0 || frame_height == 0)
    {
        LeaveCriticalSection(&cs);
        return false;
    }

    *data = frame_buffer;
    *width = frame_width;
    *height = frame_height;

    LeaveCriticalSection(&cs);
    return true;
}

// ================= STOP =================
extern "C" __declspec(dllexport)
void StopStream()
{
    if (main_loop)
    {
        g_main_loop_quit(main_loop);
        if (loop_thread)
        {
            WaitForSingleObject(loop_thread, 2000);
            CloseHandle(loop_thread);
            loop_thread = NULL;
        }
        g_main_loop_unref(main_loop);
        main_loop = NULL;
    }

    if (pipeline)
    {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
    }

    if (appsink_elem)
    {
        gst_object_unref(appsink_elem);
        appsink_elem = NULL;
    }

    EnterCriticalSection(&cs);
    free(frame_buffer);
    frame_buffer = NULL;
    frame_width = 0;
    frame_height = 0;
    buffer_size = 0;
    LeaveCriticalSection(&cs);

    DeleteCriticalSection(&cs);
    LogMsg("Pipeline stopped\n");
}

//#include <gst/gst.h>
//#include <gst/app/gstappsink.h>
//#include <windows.h>
//
//#pragma comment(lib, "user32.lib")
//
//static GstElement* pipeline = NULL;
//static GstElement* appsink = NULL;
//
//static unsigned char* frame_buffer = NULL;
//static int frame_width = 0;
//static int frame_height = 0;
//static size_t buffer_size = 0;
//
//CRITICAL_SECTION cs;
//
//
//// ================= FRAME CALLBACK =================
//static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data)
//{
//    GstSample* sample = gst_app_sink_pull_sample(sink);
//    if (!sample)
//        return GST_FLOW_ERROR;
//
//    GstBuffer* buffer = gst_sample_get_buffer(sample);
//    GstCaps* caps = gst_sample_get_caps(sample);
//
//    if (!caps)
//    {
//        gst_sample_unref(sample);
//        return GST_FLOW_ERROR;
//    }
//
//    GstStructure* s = gst_caps_get_structure(caps, 0);
//    gst_structure_get_int(s, "width", &frame_width);
//    gst_structure_get_int(s, "height", &frame_height);
//
//    GstMapInfo map;
//    if (gst_buffer_map(buffer, &map, GST_MAP_READ))
//    {
//        EnterCriticalSection(&cs);
//
//        if (buffer_size != map.size)
//        {
//            if (frame_buffer)
//                free(frame_buffer);
//
//            frame_buffer = (unsigned char*)malloc(map.size);
//            buffer_size = map.size;
//        }
//
//        if (frame_buffer)
//        {
//            memcpy(frame_buffer, map.data, map.size);
//            OutputDebugStringA("Frame received\n");
//        }
//
//        LeaveCriticalSection(&cs);
//
//        gst_buffer_unmap(buffer, &map);
//    }
//
//    gst_sample_unref(sample);
//    return GST_FLOW_OK;
//}
//
//
//// ================= START STREAM =================
//extern "C" __declspec(dllexport)
//void StartStream()
//{
//    InitializeCriticalSection(&cs);
//
//    gst_init(NULL, NULL);
//
//    const char* pipeline_str =
//        "udpsrc port=5000 caps=\"application/x-rtp, media=video, encoding-name=H264, payload=96\" ! "
//        "rtph264depay ! avdec_h264 ! "
//        "videoconvert ! video/x-raw,format=BGR,width=640,height=480 ! "
//        "appsink name=sink emit-signals=true sync=false max-buffers=1 drop=true";
//
//    GError* error = NULL;
//    pipeline = gst_parse_launch(pipeline_str, &error);
//
//    if (!pipeline)
//    {
//        OutputDebugStringA("Pipeline creation FAILED\n");
//        return;
//    }
//
//    if (error)
//    {
//        OutputDebugStringA(error->message);
//        return;
//    }
//
//    appsink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
//
//    if (!appsink)
//    {
//        OutputDebugStringA("Appsink NOT found\n");
//        return;
//    }
//
//    gst_app_sink_set_emit_signals((GstAppSink*)appsink, TRUE);
//    gst_app_sink_set_drop((GstAppSink*)appsink, TRUE);
//    gst_app_sink_set_max_buffers((GstAppSink*)appsink, 1);
//
//    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), NULL);
//
//    gst_element_set_state(pipeline, GST_STATE_PLAYING);
//
//    OutputDebugStringA("Pipeline started\n");
//}
//
//
//// ================= GET FRAME =================
//extern "C" __declspec(dllexport)
//bool GetFrame(unsigned char** data, int* width, int* height)
//{
//    EnterCriticalSection(&cs);
//
//    if (!frame_buffer || frame_width == 0 || frame_height == 0)
//    {
//        LeaveCriticalSection(&cs);
//        return false;
//    }
//
//    *data = frame_buffer;
//    *width = frame_width;
//    *height = frame_height;
//
//    LeaveCriticalSection(&cs);
//    return true;
//}
//
//
//// ================= STOP =================
//extern "C" __declspec(dllexport)
//void StopStream()
//{
//    if (pipeline)
//    {
//        gst_element_set_state(pipeline, GST_STATE_NULL);
//        gst_object_unref(pipeline);
//        pipeline = NULL;
//    }
//
//    if (frame_buffer)
//    {
//        free(frame_buffer);
//        frame_buffer = NULL;
//    }
//
//    DeleteCriticalSection(&cs);
//
//    OutputDebugStringA("Pipeline stopped\n");
//}


//#include <gst/gst.h>
//#include <gst/app/gstappsink.h>
//#include <windows.h>
//
//static GstElement* pipeline = NULL;
//static GstElement* appsink = NULL;
//
//static unsigned char* frame_buffer = NULL;
//static int frame_width = 0;
//static int frame_height = 0;
//
//extern "C" __declspec(dllexport)
//bool GetFrame(unsigned char** data, int* width, int* height)
//{
//    if (!frame_buffer)
//    {
//        OutputDebugStringA("No frame buffer yet\n");
//        return false;
//    }
//
//    *data = frame_buffer;
//    *width = frame_width;
//    *height = frame_height;
//    return true;
//}
//
//static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data)
//{
//    
//    GstSample* sample = gst_app_sink_pull_sample(sink);
//    if (!sample) return GST_FLOW_ERROR;
//
//    GstBuffer* buffer = gst_sample_get_buffer(sample);
//    GstCaps* caps = gst_sample_get_caps(sample);
//
//    if (!caps)
//    {
//        OutputDebugStringA("No caps!\n");
//        return GST_FLOW_ERROR;
//    }
//
//    GstStructure* s = gst_caps_get_structure(caps, 0);
//
//    if (!gst_structure_get_int(s, "width", &frame_width) ||
//        !gst_structure_get_int(s, "height", &frame_height))
//    {
//        OutputDebugStringA("Width/Height not found!\n");
//        return GST_FLOW_ERROR;
//    }
//
//    GstMapInfo map;
//    if (gst_buffer_map(buffer, &map, GST_MAP_READ))
//    {
//        OutputDebugStringA("Frame received\n");   // 🔥 ADD THIS
//        g_print("Frame received\n");
//        static size_t buffer_size = 0;
//
//        if (buffer_size != map.size)
//        {
//            if (frame_buffer)
//                free(frame_buffer);
//
//            frame_buffer = (unsigned char*)malloc(map.size);
//            buffer_size = map.size;
//        }
//
//        if (frame_buffer)
//        {
//            memcpy(frame_buffer, map.data, map.size);
//        }
//    }
//
//    gst_sample_unref(sample);
//    return GST_FLOW_OK;
//}
//
//extern "C" __declspec(dllexport)
//void StartStream()
//{
//    gst_init(NULL, NULL);
//
//    /*const char* pipeline_str =
//        "udpsrc port=5000 caps=\"application/x-rtp, media=video, encoding-name=H264, payload=96, clock-rate=90000\" ! "
//        "rtpjitterbuffer latency=0 ! "
//        "rtph264depay ! h264parse ! avdec_h264 ! "
//        "videoconvert ! video/x-raw,format=BGR,width=640,height=480 ! "
//        "appsink name=sink sync=false";*/
//    const char* pipeline_str =
//        "udpsrc port=5000 caps=\"application/x-rtp, media=video, encoding-name=H264, payload=96\" ! "
//        "rtph264depay ! avdec_h264 ! "
//        "videoconvert ! video/x-raw,format=BGR ! "
//        "appsink name=sink emit-signals=true sync=false max-buffers=1 drop=true";
//
//    GError* error = NULL;
//    pipeline = gst_parse_launch(pipeline_str, &error);
//    if (error)
//    {
//        OutputDebugStringA(error->message);
//        return;
//    }
//    if (!pipeline || error)
//    {
//        if (error)
//            g_print("Pipeline Error: %s\n", error->message);
//        return;
//    }
//
//    appsink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
//
//    gst_app_sink_set_emit_signals((GstAppSink*)appsink, TRUE);
//    gst_app_sink_set_drop((GstAppSink*)appsink, TRUE);
//    gst_app_sink_set_max_buffers((GstAppSink*)appsink, 1);
//    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), NULL);
//
//    gst_element_set_state(pipeline, GST_STATE_PLAYING);
//    OutputDebugStringA("Pipeline started\n");
//
//    char msg[100];
//    g_snprintf(msg, sizeof(msg), "Frame: %d x %d\n", frame_width, frame_height);
//    OutputDebugStringA(msg);
//}
//
//extern "C" __declspec(dllexport)
//void StopStream()
//{
//    if (pipeline)
//    {
//        gst_element_set_state(pipeline, GST_STATE_NULL);
//        gst_object_unref(pipeline);
//        pipeline = NULL;
//    }
//
//    if (frame_buffer)
//    {
//        free(frame_buffer);
//        frame_buffer = NULL;
//    }
//}