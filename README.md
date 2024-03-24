# MJPEG HTTP Sink GStreamer Plugin

This repository contains a custom GStreamer plugin called `mjpeghttpsink` that serves MJPEG frames over HTTP. The plugin acts as a sink element in a GStreamer pipeline, allowing you to stream MJPEG video to multiple clients via HTTP.

## Examples

Here are a few examples of how to use the `mjpeghttpsink` plugin in a GStreamer pipeline:

1. Streaming from a video file:

```
gst-launch-1.0 filesrc location=video.mp4 ! decodebin ! videoconvert ! jpegenc ! mjpeghttpsink port=8080 ! fakesink
```

2. Streaming from a live video source (e.g., webcam):

```
gst-launch-1.0 v4l2src device=/dev/video0 ! videoconvert ! jpegenc ! mjpeghttpsink ! fakesink
```

3. Streaming with additional pipeline elements:

```
gst-launch-1.0 filesrc location=video.mp4 ! decodebin ! videoconvert ! videoscale ! video/x-raw,width=640,height=480 ! jpegenc ! mjpeghttpsink ! fakesink
```

4. Using the test source:

```
gst-launch-1.0 videotestsrc ! videoconvert ! jpegenc ! mjpeghttpsink port=8081 ! fakesink
```

In these examples, the `mjpeghttpsink` plugin is used as the final sink element in the pipeline. The plugin will serve the MJPEG frames over HTTP, allowing multiple clients to connect and view the video stream.

## Building the Plugin

To build the `mjpeghttpsink` plugin, follow these steps:

1. Make sure you have the necessary dependencies installed:
   - GStreamer development libraries (version 1.0 or later)
   - GLib development libraries
   - C compiler (e.g., GCC)
   - libgstreamer-plugins-base1.0-dev

2. Clone this repository:

```bash
git clone https://github.com/heety313/gst-plugin-mjpeg-server
```

3. Navigate to the repository directory:

```
cd mjpeg-http-sink
```

4. Compile the plugin:

```bash
mkdir build
cd build
cmake ..
make
```

5. Install the plugin (optional):

```bash
#this may be different on ARM64 platforms
sudo cp libmjpeghttpsink.so /usr/local/lib/gstreamer-1.0/
```

After building the plugin, you can use it in your GStreamer pipelines by referring to it as `mjpeghttpsink`.

## Usage

To start the MJPEG HTTP server and stream video, simply include the `mjpeghttpsink` element at the end of your GStreamer pipeline. The plugin will automatically start the HTTP server and stream the MJPEG frames to connected clients.

Clients can connect to the server using a web browser or any HTTP client that supports MJPEG streaming. The default server port is 8080, `port` parameter can be used to modify the port used. 

## Contributing

Contributions to this project are welcome! If you find any issues or have suggestions for improvements, please open an issue or submit a pull request.

## License

This project is licensed under the [LGPL License](LICENSE).