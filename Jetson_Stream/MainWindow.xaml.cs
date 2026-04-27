using System;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using MessageBox = System.Windows.MessageBox;

namespace Jetson_Stream
{
    public partial class MainWindow : Window
    {
        TcpClient? _client;
        NetworkStream? _stream;
        WriteableBitmap? _bitmap;
        DispatcherTimer? _timer;

        // ==============================
        // P/INVOKE
        // FIX: GetFrame — use IntPtr* not out IntPtr.
        //      This matches unsigned char** on the C++ side.
        // ==============================
        [DllImport("gst_player.dll")] static extern void StartStream();
        [DllImport("gst_player.dll")] static extern void StopStream();

        [DllImport("gst_player.dll")]
        [return: MarshalAs(UnmanagedType.I1)]
        static extern bool GetFrame(
            out IntPtr data,    // matches unsigned char**
            out int width,
            out int height);

        public MainWindow()
        {
            InitializeComponent();
            Loaded += (s, e) => _ = ConnectAsync();
        }

        // ==============================
        // TCP — auto-reconnect loop
        // ==============================
        async Task ConnectAsync()
        {
            while (true)
            {
                try
                {
                    //SetStatus("Connecting...");
                    _client = new TcpClient();
                    await _client.ConnectAsync("192.168.10.1", 7000);
                    _stream = _client.GetStream();
                    //SetStatus("Connected");
                    await ReceiveLoopAsync();
                }
                catch
                {
                    //SetStatus("Disconnected — retrying...");
                    await Task.Delay(2000);
                }
            }
        }

        // ==============================
        // RECEIVE LOOP
        // FIX: Use a StringBuilder line buffer so split TCP
        //      packets are reassembled before JSON parsing.
        // ==============================
        async Task ReceiveLoopAsync()
        {
            var buf = new byte[4096];
            var leftover = new StringBuilder();

            while (true)
            {
                int n = await _stream!.ReadAsync(buf, 0, buf.Length);
                if (n == 0) throw new Exception("Disconnected");

                leftover.Append(Encoding.UTF8.GetString(buf, 0, n));
                string all = leftover.ToString();
                leftover.Clear();

                foreach (var raw in all.Split('\n'))
                {
                    if (string.IsNullOrWhiteSpace(raw))
                        continue;

                    // incomplete last chunk — hold for next recv
                    if (!all.EndsWith("\n") &&
                         raw == all.Split('\n')[^1])
                    {
                        leftover.Append(raw);
                        continue;
                    }

                    try
                    {
                        using var doc = JsonDocument.Parse(raw.Trim());
                        var root = doc.RootElement;

                        if (root.TryGetProperty("type", out var t) &&
                            t.GetString() == "telemetry")
                        {
                            double fps = root.GetProperty("fps").GetDouble();
                            //SetStatus($"{fps:F1}");
                        }
                    }
                    catch { /* ignore malformed line */ }
                }
            }
        }

        // ==============================
        // RENDER FRAME
        // FIX: Copy frame bytes immediately inside the lock
        //      window — don't hold the pointer across calls.
        // ==============================
        void RenderFrame(object? sender, EventArgs e)
        {
            bool got = GetFrame(out IntPtr ptr, out int w, out int h);

            // Temporary — shows in VS Output window
            System.Diagnostics.Debug.WriteLine(
                $"GetFrame: got={got}  ptr={ptr}  w={w}  h={h}");
            //if (!GetFrame(out IntPtr ptr, out int w, out int h))
            //    return;

            if (!got || w == 0 || h == 0 || ptr == IntPtr.Zero)
                return;

            int stride = w * 3;
            int size = stride * h;

            // Copy bytes OUT immediately — do not hold ptr across calls
            byte[] pixels = new byte[size];
            try
            {
                Marshal.Copy(ptr, pixels, 0, size);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Marshal.Copy failed: {ex.Message}");
                return;
            }

            // Bitmap must be created AND written on the UI thread (DispatcherTimer already is)
            if (_bitmap == null || _bitmap.PixelWidth != w || _bitmap.PixelHeight != h)
            {
                _bitmap = new WriteableBitmap(
                    w, h, 96, 96,
                    PixelFormats.Bgr24, null);

                VideoImage.Source = _bitmap;   // set source here, on UI thread
                System.Diagnostics.Debug.WriteLine($"Bitmap created: {w}x{h}");
            }

            _bitmap.Lock();
            _bitmap.WritePixels(
                new Int32Rect(0, 0, w, h),
                pixels, stride, 0);
            _bitmap.AddDirtyRect(new Int32Rect(0, 0, w, h));
            _bitmap.Unlock();
        }

        // ==============================
        // SEND COMMAND
        // ==============================
        void SendCommand(object payload)
        {
            if (_stream == null) return;
            try
            {
                string json = JsonSerializer.Serialize(payload) + "\n";
                byte[] bytes = Encoding.UTF8.GetBytes(json);
                _stream.Write(bytes, 0, bytes.Length);
            }
            catch (Exception ex)
            {
                //SetStatus($"Send error: {ex.Message}");
            }
        }

        // ==============================
        // BUTTON EVENTS
        // FIX: Start timer AFTER StartStream() returns so the
        //      pipeline is already running before we poll frames.
        // ==============================
        void Start_Click(object sender, RoutedEventArgs e)
        {
            SendCommand(new { cmd = "START" });
            StartStream();

            // Start polling — DispatcherTimer runs on UI thread, no Invoke needed
            _timer = new DispatcherTimer();
            _timer.Interval = TimeSpan.FromMilliseconds(16);  // ~60 Hz
            _timer.Tick += RenderFrame;
            _timer.Start();

            System.Diagnostics.Debug.WriteLine("Timer started");
        }

        void Pause_Click(object sender, RoutedEventArgs e)
            => SendCommand(new { cmd = "PAUSE" });

        void Stop_Click(object sender, RoutedEventArgs e)
        {
            SendCommand(new { cmd = "STOP" });
            _timer?.Stop();
            StopStream();
        }

        void ApplyParams_Click(object sender, RoutedEventArgs e)
        {
            //if (!int.TryParse(YOffsetBox.Text, out int yOffset) ||
            //    !int.TryParse(YStartBox.Text, out int yStart) ||
            //    !int.TryParse(YEndBox.Text, out int yEnd))
            //{
            //    MessageBox.Show("Enter valid integers for all params.");
            //    return;
            //}

            //SendCommand(new
            //{
            //    cmd = "SET_PARAM",
            //    data = new { Y_offset = yOffset, Y_start = yStart, Y_end = yEnd }
            //});
        }

        // ==============================
        // HELPERS
        // ==============================
        //void SetStatus(string text)
        //    => Dispatcher.Invoke(() => FpsText.Text = text);
    }
}
/*using System;
using System.Diagnostics;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using System.Timers;
using System.Windows;
using System.Windows.Forms;
using System.Windows.Forms.Integration;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using MessageBox = System.Windows.MessageBox;

namespace Jetson_Stream
{
    public partial class MainWindow : Window
    {
        TcpClient? client;
        NetworkStream? stream;
        //Process? gstProcess;
        WriteableBitmap? bitmap;
        DispatcherTimer? timer;
        [DllImport("kernel32.dll")]
        static extern bool AllocConsole();
        public MainWindow()
        {
            InitializeComponent();
            AllocConsole();
            Loaded += (s, e) =>
            {
                Connect(); // move here
            };
        }

        [DllImport("gst_player.dll")]
        static extern void StartStream();

        [DllImport("gst_player.dll")]
        static extern void StopStream();

        [DllImport("gst_player.dll")]
        static extern bool GetFrame(out IntPtr data, out int width, out int height);
        async void Connect()
        {
            while (true)
            {
                try
                {
                    Dispatcher.Invoke(() =>
                    {
                        FpsText.Text = "Connecting...";
                    });

                    client = new TcpClient();
                    await client.ConnectAsync("192.168.10.1", 7000);

                    stream = client.GetStream();

                    Dispatcher.Invoke(() =>
                    {
                        FpsText.Text = "Connected";
                    });

                    await ReceiveLoop();   // 🔥 wait here

                }
                catch (Exception)
                {
                    Dispatcher.Invoke(() =>
                    {
                        FpsText.Text = "Disconnected";
                    });

                    await Task.Delay(2000); // retry
                }
            }
        }

        async Task ReceiveLoop()
        {
            byte[] buffer = new byte[1024];

            while (true)
            {
                int bytes = await stream!.ReadAsync(buffer, 0, buffer.Length);

                if (bytes == 0) throw new Exception("Disconnected");

                string msg = Encoding.UTF8.GetString(buffer, 0, bytes);

                foreach (var line in msg.Split('\n'))
                {
                    if (string.IsNullOrWhiteSpace(line)) continue;

                    var json = JsonDocument.Parse(line);

                    if (json.RootElement.TryGetProperty("type", out var type) &&
                        type.GetString() == "telemetry")
                    {
                        double fps = json.RootElement.GetProperty("fps").GetDouble();

                        Dispatcher.Invoke(() =>
                        {
                            FpsText.Text = fps.ToString("F1");
                        });
                    }
                }
            }
        }

        void SendCommand(object obj)
        {
            string json = JsonSerializer.Serialize(obj);
            byte[] data = Encoding.UTF8.GetBytes(json);
            stream!.Write(data, 0, data.Length);
        }

        // ==============================
        // BUTTON EVENTS
        // ==============================
        
        private void Start_Click(object sender, RoutedEventArgs e)
        {
            SendCommand(new { cmd = "START" });

            StartStream();

            timer = new DispatcherTimer();
            timer.Interval = TimeSpan.FromMilliseconds(30);
            timer.Tick += RenderFrame;
            timer.Start();
        }

        private void Pause_Click(object sender, RoutedEventArgs e)
        {
            SendCommand(new { cmd = "PAUSE" });
        }

        private void Stop_Click(object sender, RoutedEventArgs e)
        {
            SendCommand(new { cmd = "STOP" });

            timer?.Stop();
            StopStream();
        }
        void RenderFrame(object? sender, EventArgs e)
        {
            if (!GetFrame(out IntPtr ptr, out int w, out int h))
                return;

            // 🔥 ADD THIS HERE
            if (w == 0 || h == 0)
                return;

            if (bitmap == null)
            {
                bitmap = new WriteableBitmap(
                    w, h, 96, 96,
                    PixelFormats.Bgr24, null);

                VideoImage.Source = bitmap;
            }

            bitmap.Lock();

            int size = w * h * 3;
            byte[] buffer = new byte[size];

            Marshal.Copy(ptr, buffer, 0, size);

            bitmap.WritePixels(
                new Int32Rect(0, 0, w, h),
                buffer,
                w * 3,
                0);

            bitmap.AddDirtyRect(new Int32Rect(0, 0, w, h));
            bitmap.Unlock();
        }

        private void ApplyParams_Click(object sender, RoutedEventArgs e)
        {
                SendCommand(new
            {
                cmd = "SET_PARAM",
                data = new
                {
                    Y_offset = int.Parse(YOffsetBox.Text),
                    Y_start = int.Parse(YStartBox.Text),
                    Y_end = int.Parse(YEndBox.Text)
                }
            });
        }
    }
}*/