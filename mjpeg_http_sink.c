#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <glib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

#define DEFAULT_PORT 8080
#define BUFFER_SIZE 4096
#define NUM_BUFFERS 20

#ifndef PACKAGE
#define PACKAGE "mjpeghttpsink"
#endif

#define GST_TYPE_MJPEG_HTTP_SINK (gst_mjpeg_http_sink_get_type())
#define GST_MJPEG_HTTP_SINK(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_MJPEG_HTTP_SINK, MjpegHttpSink))

int active_slice;
char *shared_buffer_slice[NUM_BUFFERS];
int shared_buffer_slice_size[NUM_BUFFERS];

enum
{
    PROP_0,
    PROP_PORT
};

typedef struct _MjpegHttpSink
{
    GstBaseTransform parent;
    GstBuffer *buffer;
    GMutex mutex;
    GThread *thread;
    GThread *image_grabber_thread;
    gint port;
} MjpegHttpSink;

typedef struct _MjpegHttpSinkClass
{
    GstBaseTransformClass parent_class;
} MjpegHttpSinkClass;

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
                                                                    GST_PAD_SINK,
                                                                    GST_PAD_ALWAYS,
                                                                    GST_STATIC_CAPS("image/jpeg"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src",
                                                                   GST_PAD_SRC,
                                                                   GST_PAD_ALWAYS,
                                                                   GST_STATIC_CAPS("image/jpeg"));

#define gst_mjpeg_http_sink_parent_class parent_class
G_DEFINE_TYPE(MjpegHttpSink, gst_mjpeg_http_sink, GST_TYPE_BASE_TRANSFORM);

static void gst_mjpeg_http_sink_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    MjpegHttpSink *sink = GST_MJPEG_HTTP_SINK(object);

    switch (prop_id)
    {
    case PROP_PORT:
        sink->port = g_value_get_int(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void gst_mjpeg_http_sink_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    MjpegHttpSink *sink = GST_MJPEG_HTTP_SINK(object);

    switch (prop_id)
    {
    case PROP_PORT:
        g_value_set_int(value, sink->port);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

void sigpipe_handler(int signum)
{
    pthread_exit(NULL);
}

void *handle_connection(void *arg)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigpipe_handler;
    if (sigaction(SIGPIPE, &sa, NULL) == -1)
    {
        perror("Error setting signal handler");
        pthread_exit(NULL);
    }

    int newsockfd = *(int *)arg;
    char buffer[1024];

    // Send HTTP headers
    snprintf(buffer, sizeof(buffer), "HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n");
    if (send(newsockfd, buffer, strlen(buffer), 0) == -1)
    {
        perror("Error sending HTTP headers");
        close(newsockfd);
        free(arg);
        pthread_exit(NULL);
    }

    int previous_slice = 0;

    // Read and send MJPEG frames
    while (1)
    {
        char dummy;
        ssize_t recv_bytes = recv(newsockfd, &dummy, 1, MSG_PEEK);
        if (recv_bytes == -1)
        {
            perror("Error receiving data");
            break;
        }
        else if (recv_bytes == 0)
        {
            printf("Client disconnected\n");
            break;
        }

        if (previous_slice == active_slice)
        {
            usleep(1000);
            continue;
        }

        // Send the frame over HTTP
        snprintf(buffer, sizeof(buffer), "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", shared_buffer_slice_size[active_slice]);
        if (send(newsockfd, buffer, strlen(buffer), 0) == -1)
        {
            perror("Error sending frame header");
            break;
        }

        if (send(newsockfd, shared_buffer_slice[active_slice], shared_buffer_slice_size[active_slice], 0) == -1)
        {
            perror("Error sending frame data");
            break;
        }

        previous_slice = active_slice;
    }

    // Close the client socket
    close(newsockfd);
    free(arg);
    return NULL;
}

static gpointer image_grabber_thread(gpointer data)
{
    MjpegHttpSink *sink = (MjpegHttpSink *)data;

    // Allocate memory for the shared buffer slices
    for (int i = 0; i < NUM_BUFFERS; i++)
    {
        shared_buffer_slice[i] = malloc(4096000);
        if (shared_buffer_slice[i] == NULL)
        {
            perror("Error allocating memory for shared buffer slice");
            pthread_exit(NULL);
        }
        shared_buffer_slice_size[i] = 0;
    }

    int which_slice = 0;
    GstClockTime timestamp;
    GstClockTime prev_timestamp = 0;

    while (1)
    {
        g_mutex_lock(&sink->mutex);
        if (sink->buffer != NULL)
        {
            timestamp = GST_BUFFER_TIMESTAMP(sink->buffer);
            if (timestamp == prev_timestamp)
            {
                g_mutex_unlock(&sink->mutex);
                usleep(1000);
                continue;
            }
            prev_timestamp = timestamp;

            GstMapInfo map;
            if (gst_buffer_map(sink->buffer, &map, GST_MAP_READ))
            {
                if (which_slice > NUM_BUFFERS - 1)
                {
                    which_slice = 0;
                }

                // Copy the buffer to the shared buffer
                memcpy(shared_buffer_slice[which_slice], map.data, map.size);
                shared_buffer_slice_size[which_slice] = map.size;
                active_slice = which_slice;
                which_slice++;

                gst_buffer_unmap(sink->buffer, &map);
            }
            else
            {
                g_warning("Failed to map buffer");
            }
        }
        g_mutex_unlock(&sink->mutex);
    }

    return NULL;
}

static gpointer http_server_thread(gpointer data)
{
    int sockfd, ret;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    pthread_t grab_thread;

    MjpegHttpSink *sink = (MjpegHttpSink *)data;
    int port = sink->port;

    // Create a socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
    {
        perror("Error creating socket");
        pthread_exit(NULL);
    }

    // Set socket options to reuse address and port
    int optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &optval, sizeof(optval)) == -1)
    {
        perror("Error setting socket options");
        close(sockfd);
        pthread_exit(NULL);
    }

    // Bind the socket to a port
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("Error binding socket");
        close(sockfd);
        pthread_exit(NULL);
    }

    // Listen for incoming connections
    if (listen(sockfd, 5) == -1)
    {
        perror("Error listening on socket");
        close(sockfd);
        pthread_exit(NULL);
    }

    while (1)
    {
        // Accept a client connection
        client_len = sizeof(client_addr);
        int *newsockfd = (int *)malloc(sizeof(int));
        if (newsockfd == NULL)
        {
            perror("Error allocating memory for client socket");
            continue;
        }

        *newsockfd = accept(sockfd, (struct sockaddr *)&client_addr, &client_len);
        if (*newsockfd == -1)
        {
            perror("Error accepting connection");
            free(newsockfd);
            continue;
        }

        // Create a new thread to handle the connection
        pthread_t connection_thread;
        if (pthread_create(&connection_thread, NULL, handle_connection, newsockfd) != 0)
        {
            perror("Error creating connection thread");
            close(*newsockfd);
            free(newsockfd);
        }
        else
        {
            pthread_detach(connection_thread);
        }
    }

    // Close the server socket
    close(sockfd);
    return NULL;
}

static GstFlowReturn gst_mjpeg_http_sink_transform_ip(GstBaseTransform *trans, GstBuffer *buf)
{
    MjpegHttpSink *sink = (MjpegHttpSink *)trans;

    g_mutex_lock(&sink->mutex);
    if (sink->buffer != NULL)
        gst_buffer_unref(sink->buffer);
    sink->buffer = gst_buffer_ref(buf);
    g_mutex_unlock(&sink->mutex);

    return GST_FLOW_OK;
}

static gboolean gst_mjpeg_http_sink_start(GstBaseTransform *trans)
{
    MjpegHttpSink *sink = (MjpegHttpSink *)trans;

    g_mutex_init(&sink->mutex);

    sink->thread = g_thread_new("http-server-thread", http_server_thread, sink);
    if (sink->thread == NULL)
    {
        g_warning("Failed to create HTTP server thread");
        return FALSE;
    }

    sink->image_grabber_thread = g_thread_new("image-grabber-thread", image_grabber_thread, sink);
    if (sink->image_grabber_thread == NULL)
    {
        g_warning("Failed to create image grabber thread");
        g_thread_join(sink->thread);
        return FALSE;
    }

    return TRUE;
}

static gboolean gst_mjpeg_http_sink_stop(GstBaseTransform *trans)
{
    MjpegHttpSink *sink = (MjpegHttpSink *)trans;

    g_thread_join(sink->thread);
    g_thread_join(sink->image_grabber_thread);

    g_mutex_clear(&sink->mutex);

    if (sink->buffer != NULL)
    {
        gst_buffer_unref(sink->buffer);
        sink->buffer = NULL;
    }

    return TRUE;
}

static void gst_mjpeg_http_sink_class_init(MjpegHttpSinkClass *klass)
{
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass *base_transform_class = GST_BASE_TRANSFORM_CLASS(klass);
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&sink_template));
    gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&src_template));

    gst_element_class_set_static_metadata(element_class,
                                          "MJPEG HTTP Sink",
                                          "Sink/Network",
                                          "Serves MJPEG frames over HTTP",
                                          "Your Name <your.email@example.com>");

    base_transform_class->transform_ip = GST_DEBUG_FUNCPTR(gst_mjpeg_http_sink_transform_ip);
    base_transform_class->start = GST_DEBUG_FUNCPTR(gst_mjpeg_http_sink_start);
    base_transform_class->stop = GST_DEBUG_FUNCPTR(gst_mjpeg_http_sink_stop);

    gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_mjpeg_http_sink_set_property);
    gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_mjpeg_http_sink_get_property);

    g_object_class_install_property(gobject_class, PROP_PORT,
                                    g_param_spec_int("port", "Port", "The port number to listen on",
                                                     0, G_MAXUINT16, DEFAULT_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void gst_mjpeg_http_sink_init(MjpegHttpSink *sink)
{
    sink->buffer = NULL;
    sink->port = DEFAULT_PORT;
}

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "mjpeghttpsink", GST_RANK_NONE, GST_TYPE_MJPEG_HTTP_SINK);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    mjpeghttpsink,
    "MJPEG HTTP Sink",
    plugin_init,
    "1.0",
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/")