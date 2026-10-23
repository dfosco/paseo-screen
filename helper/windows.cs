// paseo-screen-helper for Windows (experimental): captures the screen with GDI and injects input with
// SendInput for the paseo-screen plugin. Compiled on the host with the .NET Framework's csc.exe, which
// accepts C# 5 only, so this file avoids newer syntax.
//
// Protocol: one JSON object per line on stdin, one JSON reply per line on stdout.
//   {"id": 1, "cmd": "status"}
//   {"id": 2, "cmd": "frame", "display": 1, "maxWidth": 1600, "quality": 0.6, "since": "abc"}
//   {"id": 3, "cmd": "input", "display": 1, "events": [{"type": "click", "x": 0.5, "y": 0.5}]}
//   {"id": 4, "cmd": "clipboard"}
// Coordinates in input events are normalized to 0...1 within the chosen display. Key codes are macOS
// virtual key codes (physical key positions) and are pressed by scan code, so the host's layout decides
// the character. The "cmd" modifier is the Windows key.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace PaseoScreen
{
    class HelperError : Exception
    {
        public HelperError(string message) : base(message) { }
    }

    class DisplayInfo
    {
        public int Id;
        public Rectangle Bounds;
        public bool Primary;
        public string Name;

        public Dictionary<string, object> Json()
        {
            var json = new Dictionary<string, object>();
            json["id"] = Id;
            json["x"] = Bounds.X;
            json["y"] = Bounds.Y;
            // The process is per-monitor DPI aware, so bounds are physical pixels; points equal pixels.
            json["width"] = Bounds.Width;
            json["height"] = Bounds.Height;
            json["pixelWidth"] = Bounds.Width;
            json["pixelHeight"] = Bounds.Height;
            json["main"] = Primary;
            json["builtin"] = false;
            json["name"] = Name;
            return json;
        }
    }

    static class Program
    {
        static readonly JavaScriptSerializer Json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
        static StreamWriter output;

        [STAThread]
        static int Main()
        {
            Native.EnableDpiAwareness();
            output = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false));
            output.AutoFlush = true;
            var input = new StreamReader(Console.OpenStandardInput(), new UTF8Encoding(false));
            string line;
            while ((line = input.ReadLine()) != null)
            {
                if (line.Length == 0) continue;
                Handle(line);
            }
            return 0;
        }

        static void Reply(Dictionary<string, object> reply)
        {
            output.Write(Json.Serialize(reply));
            output.Write("\n");
        }

        static void Handle(string line)
        {
            Dictionary<string, object> request;
            try
            {
                request = Json.Deserialize<Dictionary<string, object>>(line);
            }
            catch (Exception)
            {
                request = null;
            }
            if (request == null)
            {
                var invalid = new Dictionary<string, object>();
                invalid["ok"] = false;
                invalid["error"] = "invalid JSON request";
                Reply(invalid);
                return;
            }
            object id = request.ContainsKey("id") ? request["id"] : null;
            Dictionary<string, object> result;
            try
            {
                string command = Str(request, "cmd");
                switch (command)
                {
                    case "status":
                    case "request":
                        result = Status();
                        break;
                    case "frame":
                        result = Capture.Frame(request);
                        break;
                    case "input":
                        result = Input.Handle(request);
                        break;
                    case "clipboard":
                        result = ClipboardText.Read();
                        break;
                    default:
                        throw new HelperError("unknown command: " + (command ?? "nil"));
                }
                result["ok"] = true;
            }
            catch (Exception error)
            {
                result = new Dictionary<string, object>();
                result["ok"] = false;
                result["error"] = error is HelperError ? error.Message : error.GetType().Name + ": " + error.Message;
            }
            result["id"] = id;
            Reply(result);
        }

        static Dictionary<string, object> Status()
        {
            var displays = Displays.List();
            var status = new Dictionary<string, object>();
            // Windows has no screen-capture or input permission for desktop apps.
            status["screenRecording"] = true;
            status["accessibility"] = true;
            status["locked"] = Native.SecureDesktopShowing();
            var list = new List<object>();
            foreach (var display in displays) list.Add(display.Json());
            status["displays"] = list;
            var active = Displays.Containing(Native.ForegroundWindowCenter(), displays);
            if (active != null) status["activeDisplay"] = active.Id;
            return status;
        }

        // MARK: JSON helpers

        public static string Str(Dictionary<string, object> map, string key)
        {
            object value;
            return map != null && map.TryGetValue(key, out value) ? value as string : null;
        }

        public static double Num(Dictionary<string, object> map, string key, double fallback)
        {
            object value;
            if (map == null || !map.TryGetValue(key, out value) || value == null || value is string || value is bool) return fallback;
            try { return Convert.ToDouble(value); } catch (Exception) { return fallback; }
        }

        public static bool Bool(Dictionary<string, object> map, string key)
        {
            object value;
            return map != null && map.TryGetValue(key, out value) && value is bool && (bool)value;
        }

        public static bool Has(Dictionary<string, object> map, string key)
        {
            object value;
            return map != null && map.TryGetValue(key, out value) && value != null;
        }
    }

    // MARK: Displays

    static class Displays
    {
        public static List<DisplayInfo> List()
        {
            var displays = new List<DisplayInfo>();
            Native.MonitorEnumProc callback = delegate (IntPtr monitor, IntPtr hdc, ref Native.RECT rect, IntPtr data)
            {
                var info = new Native.MONITORINFOEX();
                info.cbSize = Marshal.SizeOf(typeof(Native.MONITORINFOEX));
                if (Native.GetMonitorInfo(monitor, ref info))
                {
                    var bounds = Rectangle.FromLTRB(info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom);
                    displays.Add(new DisplayInfo
                    {
                        Id = DisplayNumber(info.szDevice, displays.Count + 1),
                        Bounds = bounds,
                        Primary = (info.dwFlags & 1) != 0,
                        Name = FriendlyName(info.szDevice, displays.Count + 1),
                    });
                }
                return true;
            };
            Native.EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, callback, IntPtr.Zero);
            GC.KeepAlive(callback);
            return displays;
        }

        /// "\\.\DISPLAY2" is display 2; the number stays the same while the monitor is connected.
        static int DisplayNumber(string device, int fallback)
        {
            if (device == null) return fallback;
            int start = device.Length;
            while (start > 0 && char.IsDigit(device[start - 1])) start--;
            int number;
            return start < device.Length && int.TryParse(device.Substring(start), out number) ? number : fallback;
        }

        static string FriendlyName(string device, int index)
        {
            var monitor = new Native.DISPLAY_DEVICE();
            monitor.cb = Marshal.SizeOf(typeof(Native.DISPLAY_DEVICE));
            if (device != null && Native.EnumDisplayDevices(device, 0, ref monitor, 0) && !string.IsNullOrEmpty(monitor.DeviceString))
            {
                return monitor.DeviceString.Trim();
            }
            return "Display " + index;
        }

        public static DisplayInfo Resolve(object requested, List<DisplayInfo> displays)
        {
            if (displays.Count == 0) throw new HelperError("no display is connected");
            if (requested != null && !(requested is string) && !(requested is bool))
            {
                int id = Convert.ToInt32(requested);
                foreach (var display in displays) if (display.Id == id) return display;
            }
            foreach (var display in displays) if (display.Primary) return display;
            return displays[0];
        }

        public static DisplayInfo Containing(Point point, List<DisplayInfo> displays)
        {
            foreach (var display in displays) if (display.Bounds.Contains(point)) return display;
            return null;
        }

        /// The nearest point on any display, so a drag past the outer edge of the desktop stops there.
        public static Point ClampToDesktop(double x, double y, List<DisplayInfo> displays)
        {
            var best = new Point((int)Math.Round(x), (int)Math.Round(y));
            double bestDistance = double.MaxValue;
            foreach (var display in displays)
            {
                var b = display.Bounds;
                double cx = Math.Min(Math.Max(x, b.Left), b.Right - 1);
                double cy = Math.Min(Math.Max(y, b.Top), b.Bottom - 1);
                double distance = Math.Sqrt((cx - x) * (cx - x) + (cy - y) * (cy - y));
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = new Point((int)Math.Round(cx), (int)Math.Round(cy));
                }
            }
            return best;
        }
    }

    // MARK: Capture

    static class Capture
    {
        static ImageCodecInfo jpegCodec;

        public static Dictionary<string, object> Frame(Dictionary<string, object> request)
        {
            if (Program.Bool(request, "wake")) Native.WakeDisplay();
            var displays = Displays.List();
            var display = Displays.Resolve(request.ContainsKey("display") ? request["display"] : null, displays);
            int maxWidth = Math.Max(320, (int)Program.Num(request, "maxWidth", 1600));
            double quality = Math.Min(1, Math.Max(0.1, Program.Num(request, "quality", 0.6)));
            bool locked = Native.SecureDesktopShowing();

            byte[] jpeg;
            int imageWidth, imageHeight;
            try
            {
                jpeg = Grab(display.Bounds, maxWidth, quality, out imageWidth, out imageHeight);
            }
            catch (Exception)
            {
                // The lock screen and UAC prompts run on a secure desktop that this process cannot read.
                if (!locked) throw;
                var blocked = new Dictionary<string, object>();
                blocked["locked"] = true;
                return blocked;
            }

            string hash = Fnv1a(jpeg);
            var result = new Dictionary<string, object>();
            result["display"] = display.Json();
            result["imageWidth"] = imageWidth;
            result["imageHeight"] = imageHeight;
            result["hash"] = hash;
            result["bytes"] = jpeg.Length;
            result["locked"] = locked;
            if (Program.Str(request, "since") == hash) result["unchanged"] = true;
            else result["jpeg"] = Convert.ToBase64String(jpeg);
            return result;
        }

        static byte[] Grab(Rectangle bounds, int maxWidth, double quality, out int width, out int height)
        {
            using (var full = new Bitmap(bounds.Width, bounds.Height, PixelFormat.Format24bppRgb))
            {
                using (var graphics = Graphics.FromImage(full))
                {
                    graphics.CopyFromScreen(bounds.Left, bounds.Top, 0, 0, bounds.Size, CopyPixelOperation.SourceCopy);
                    DrawCursor(graphics, bounds);
                }
                double scale = Math.Min(1.0, (double)maxWidth / bounds.Width);
                width = Math.Max(1, (int)(bounds.Width * scale));
                height = Math.Max(1, (int)(bounds.Height * scale));
                if (width == bounds.Width) return Encode(full, quality);
                using (var scaled = new Bitmap(width, height, PixelFormat.Format24bppRgb))
                {
                    using (var graphics = Graphics.FromImage(scaled))
                    {
                        graphics.InterpolationMode = InterpolationMode.HighQualityBilinear;
                        graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
                        graphics.DrawImage(full, 0, 0, width, height);
                    }
                    return Encode(scaled, quality);
                }
            }
        }

        /// CopyFromScreen leaves out the mouse pointer, so draw it in, as the macOS and Linux helpers show it.
        static void DrawCursor(Graphics graphics, Rectangle bounds)
        {
            var info = new Native.CURSORINFO();
            info.cbSize = Marshal.SizeOf(typeof(Native.CURSORINFO));
            if (!Native.GetCursorInfo(ref info) || (info.flags & 1) == 0 || info.hCursor == IntPtr.Zero) return;
            if (!bounds.Contains(info.ptScreenPos.x, info.ptScreenPos.y)) return;
            Native.ICONINFO icon;
            int hotX = 0, hotY = 0;
            if (Native.GetIconInfo(info.hCursor, out icon))
            {
                hotX = icon.xHotspot;
                hotY = icon.yHotspot;
                if (icon.hbmMask != IntPtr.Zero) Native.DeleteObject(icon.hbmMask);
                if (icon.hbmColor != IntPtr.Zero) Native.DeleteObject(icon.hbmColor);
            }
            IntPtr hdc = graphics.GetHdc();
            try
            {
                Native.DrawIconEx(hdc, info.ptScreenPos.x - bounds.Left - hotX, info.ptScreenPos.y - bounds.Top - hotY, info.hCursor, 0, 0, 0, IntPtr.Zero, 3);
            }
            finally
            {
                graphics.ReleaseHdc(hdc);
            }
        }

        static byte[] Encode(Bitmap bitmap, double quality)
        {
            if (jpegCodec == null)
            {
                foreach (var codec in ImageCodecInfo.GetImageEncoders())
                {
                    if (codec.MimeType == "image/jpeg") jpegCodec = codec;
                }
                if (jpegCodec == null) throw new HelperError("no JPEG encoder is available");
            }
            using (var parameters = new EncoderParameters(1))
            using (var stream = new MemoryStream())
            {
                parameters.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, (long)Math.Round(quality * 100));
                bitmap.Save(stream, jpegCodec, parameters);
                return stream.ToArray();
            }
        }

        static string Fnv1a(byte[] data)
        {
            ulong hash = 0xcbf29ce484222325UL;
            foreach (byte value in data)
            {
                hash ^= value;
                hash = unchecked(hash * 0x100000001b3UL);
            }
            return hash.ToString("x");
        }
    }

    // MARK: Input

    static class Input
    {
        /// macOS virtual key code to PC scan code; 0xE0xx marks an extended key.
        static readonly Dictionary<int, int> ScanCodes = new Dictionary<int, int>
        {
            {0, 0x1E}, {1, 0x1F}, {2, 0x20}, {3, 0x21}, {4, 0x23}, {5, 0x22}, {6, 0x2C}, {7, 0x2D}, {8, 0x2E}, {9, 0x2F},
            {10, 0x56}, {11, 0x30}, {12, 0x10}, {13, 0x11}, {14, 0x12}, {15, 0x13}, {16, 0x15}, {17, 0x14},
            {18, 0x02}, {19, 0x03}, {20, 0x04}, {21, 0x05}, {22, 0x07}, {23, 0x06}, {24, 0x0D}, {25, 0x0A},
            {26, 0x08}, {27, 0x0C}, {28, 0x09}, {29, 0x0B}, {30, 0x1B}, {31, 0x18}, {32, 0x16}, {33, 0x1A},
            {34, 0x17}, {35, 0x19}, {36, 0x1C}, {37, 0x26}, {38, 0x24}, {39, 0x28}, {40, 0x25}, {41, 0x27},
            {42, 0x2B}, {43, 0x33}, {44, 0x35}, {45, 0x31}, {46, 0x32}, {47, 0x34}, {48, 0x0F}, {49, 0x39},
            {50, 0x29}, {51, 0x0E}, {53, 0x01},
            {54, 0xE05C}, {55, 0xE05B}, {56, 0x2A}, {57, 0x3A}, {58, 0x38}, {59, 0x1D}, {60, 0x36}, {61, 0xE038}, {62, 0xE01D},
            {65, 0x53}, {67, 0x37}, {69, 0x4E}, {71, 0x45}, {75, 0xE035}, {76, 0xE01C}, {78, 0x4A},
            {82, 0x52}, {83, 0x4F}, {84, 0x50}, {85, 0x51}, {86, 0x4B}, {87, 0x4C}, {88, 0x4D}, {89, 0x47}, {91, 0x48}, {92, 0x49},
            {122, 0x3B}, {120, 0x3C}, {99, 0x3D}, {118, 0x3E}, {96, 0x3F}, {97, 0x40}, {98, 0x41}, {100, 0x42},
            {101, 0x43}, {109, 0x44}, {103, 0x57}, {111, 0x58},
            {114, 0xE052}, {115, 0xE047}, {116, 0xE049}, {117, 0xE053}, {119, 0xE04F}, {121, 0xE051},
            {123, 0xE04B}, {124, 0xE04D}, {125, 0xE050}, {126, 0xE048},
        };

        /// US scan codes for letters a layout cannot produce, matching the macOS and Linux helpers.
        static readonly Dictionary<char, int> UsLetters = new Dictionary<char, int>
        {
            {'a', 0x1E}, {'b', 0x30}, {'c', 0x2E}, {'d', 0x20}, {'e', 0x12}, {'f', 0x21}, {'g', 0x22}, {'h', 0x23}, {'i', 0x17},
            {'j', 0x24}, {'k', 0x25}, {'l', 0x26}, {'m', 0x32}, {'n', 0x31}, {'o', 0x18}, {'p', 0x19}, {'q', 0x10}, {'r', 0x13},
            {'s', 0x1F}, {'t', 0x14}, {'u', 0x16}, {'v', 0x2F}, {'w', 0x11}, {'x', 0x2D}, {'y', 0x15}, {'z', 0x2C},
        };

        // Modifier scan codes, pressed in this order and released in reverse.
        const int ScanCtrl = 0x1D, ScanAlt = 0x38, ScanShift = 0x2A, ScanWin = 0xE05B;

        static Point? lastPointer;

        public static Dictionary<string, object> Handle(Dictionary<string, object> request)
        {
            var displays = Displays.List();
            var display = Displays.Resolve(request.ContainsKey("display") ? request["display"] : null, displays);
            var events = request.ContainsKey("events") ? request["events"] as IList : null;
            int handled = 0;
            if (events != null)
            {
                foreach (object item in events)
                {
                    var e = item as Dictionary<string, object>;
                    if (e == null) continue;
                    Apply(e, display, displays);
                    handled++;
                }
            }
            var result = new Dictionary<string, object>();
            result["handled"] = handled;
            if (lastPointer.HasValue)
            {
                var landed = Displays.Containing(lastPointer.Value, displays);
                if (landed != null) result["pointerDisplay"] = landed.Id;
            }
            return result;
        }

        static void Apply(Dictionary<string, object> e, DisplayInfo display, List<DisplayInfo> displays)
        {
            var modifiers = Modifiers(e);
            string type = Program.Str(e, "type");
            switch (type)
            {
                case "move":
                    MoveTo(PointOf(e, display, displays));
                    break;
                case "down":
                    {
                        string button = Button(e);
                        MoveTo(PointOf(e, display, displays));
                        WithModifiers(modifiers, delegate { Mouse(ButtonFlag(button, true), 0); });
                        break;
                    }
                case "up":
                    {
                        string button = Button(e);
                        MoveTo(PointOf(e, display, displays));
                        WithModifiers(modifiers, delegate { Mouse(ButtonFlag(button, false), 0); });
                        break;
                    }
                case "click":
                    {
                        string button = Button(e);
                        int count = Math.Max(1, Math.Min(3, (int)Program.Num(e, "count", 1)));
                        MoveTo(PointOf(e, display, displays));
                        WithModifiers(modifiers, delegate
                        {
                            // Windows counts a double-click from timing, so the presses go out back to back.
                            for (int i = 0; i < count; i++)
                            {
                                Mouse(ButtonFlag(button, true), 0);
                                Mouse(ButtonFlag(button, false), 0);
                            }
                        });
                        break;
                    }
                case "scroll":
                    {
                        if (Program.Has(e, "x")) MoveTo(PointOf(e, display, displays));
                        // macOS pixel deltas; a wheel notch (120) scrolls about 100 pixels.
                        double dx = Math.Max(-10000, Math.Min(10000, Program.Num(e, "dx", 0)));
                        double dy = Math.Max(-10000, Math.Min(10000, Program.Num(e, "dy", 0)));
                        if (dy != 0) Mouse(Native.MOUSEEVENTF_WHEEL, (int)Math.Round(-dy * 1.2));
                        if (dx != 0) Mouse(Native.MOUSEEVENTF_HWHEEL, (int)Math.Round(dx * 1.2));
                        break;
                    }
                case "key":
                    {
                        int scan = ResolveKey(e, modifiers.Contains("cmd") || modifiers.Contains("ctrl"));
                        if (scan < 0) throw new HelperError("no key for " + (Program.Str(e, "char") ?? Program.Num(e, "keyCode", -1).ToString()) + " on this keyboard layout");
                        object down;
                        bool hasDown = e.TryGetValue("down", out down) && down is bool;
                        WithModifiers(modifiers, delegate
                        {
                            if (hasDown)
                            {
                                Key(scan, (bool)down);
                            }
                            else
                            {
                                Key(scan, true);
                                Key(scan, false);
                            }
                        });
                        break;
                    }
                case "text":
                    TypeText(Program.Str(e, "text") ?? "");
                    break;
                case "paste":
                    Paste(Program.Str(e, "text") ?? "");
                    break;
                default:
                    throw new HelperError("unknown input event: " + (type ?? "nil"));
            }
        }

        static string Button(Dictionary<string, object> e)
        {
            string button = Program.Str(e, "button");
            return button == "right" || button == "middle" ? button : "left";
        }

        static uint ButtonFlag(string button, bool down)
        {
            if (button == "right") return down ? Native.MOUSEEVENTF_RIGHTDOWN : Native.MOUSEEVENTF_RIGHTUP;
            if (button == "middle") return down ? Native.MOUSEEVENTF_MIDDLEDOWN : Native.MOUSEEVENTF_MIDDLEUP;
            return down ? Native.MOUSEEVENTF_LEFTDOWN : Native.MOUSEEVENTF_LEFTUP;
        }

        static List<string> Modifiers(Dictionary<string, object> e)
        {
            var names = new List<string>();
            var list = e.ContainsKey("modifiers") ? e["modifiers"] as IList : null;
            if (list == null) return names;
            foreach (object item in list)
            {
                var name = item as string;
                if (name == "meta" || name == "super") name = "cmd";
                if (name == "option") name = "alt";
                if (name == "control") name = "ctrl";
                if (name != null && !names.Contains(name)) names.Add(name);
            }
            return names;
        }

        /// Maps normalized coordinates on `display` to the global desktop. Values outside 0...1 (a drag that
        /// left the viewed display) continue onto neighbouring displays and stop at the desktop's outer edge.
        static Point PointOf(Dictionary<string, object> e, DisplayInfo display, List<DisplayInfo> displays)
        {
            double x = Program.Num(e, "x", 0.5), y = Program.Num(e, "y", 0.5);
            var b = display.Bounds;
            var point = Displays.ClampToDesktop(b.Left + x * (b.Width - 1), b.Top + y * (b.Height - 1), displays);
            lastPointer = point;
            return point;
        }

        static void MoveTo(Point point)
        {
            int left = Native.GetSystemMetrics(76), top = Native.GetSystemMetrics(77);
            int width = Math.Max(2, Native.GetSystemMetrics(78)), height = Math.Max(2, Native.GetSystemMetrics(79));
            var input = new Native.INPUT();
            input.type = Native.INPUT_MOUSE;
            input.u.mi.dx = (int)Math.Round((point.X - left) * 65535.0 / (width - 1));
            input.u.mi.dy = (int)Math.Round((point.Y - top) * 65535.0 / (height - 1));
            input.u.mi.dwFlags = Native.MOUSEEVENTF_MOVE | Native.MOUSEEVENTF_ABSOLUTE | Native.MOUSEEVENTF_VIRTUALDESK;
            Native.Send(input);
        }

        static void Mouse(uint flags, int data)
        {
            var input = new Native.INPUT();
            input.type = Native.INPUT_MOUSE;
            input.u.mi.dwFlags = flags;
            input.u.mi.mouseData = unchecked((uint)data);
            Native.Send(input);
        }

        static void Key(int scan, bool down)
        {
            var input = new Native.INPUT();
            input.type = Native.INPUT_KEYBOARD;
            input.u.ki.wScan = (ushort)(scan & 0xFF);
            uint flags = Native.KEYEVENTF_SCANCODE;
            if ((scan & 0xE000) == 0xE000) flags |= Native.KEYEVENTF_EXTENDEDKEY;
            if (!down) flags |= Native.KEYEVENTF_KEYUP;
            input.u.ki.dwFlags = flags;
            Native.Send(input);
        }

        static void WithModifiers(List<string> modifiers, ThreadStart body)
        {
            var held = new List<int>();
            if (modifiers.Contains("ctrl")) held.Add(ScanCtrl);
            if (modifiers.Contains("alt")) held.Add(ScanAlt);
            if (modifiers.Contains("shift")) held.Add(ScanShift);
            if (modifiers.Contains("cmd")) held.Add(ScanWin);
            foreach (int scan in held) Key(scan, true);
            try
            {
                body();
            }
            finally
            {
                for (int i = held.Count - 1; i >= 0; i--) Key(held[i], false);
            }
        }

        /// A physical key from `keyCode`, or the key that types `char` on the host's layout.
        static int ResolveKey(Dictionary<string, object> e, bool shortcut)
        {
            string text = Program.Str(e, "char");
            if (!string.IsNullOrEmpty(text))
            {
                char character = char.ToLowerInvariant(text[0]);
                short packed = Native.VkKeyScan(character);
                if (packed != -1)
                {
                    uint scan = Native.MapVirtualKey((uint)(packed & 0xFF), 0);
                    if (scan != 0) return (int)scan;
                }
                int fallback;
                return UsLetters.TryGetValue(character, out fallback) ? fallback : -1;
            }
            int code = (int)Program.Num(e, "keyCode", -1);
            int mapped;
            return ScanCodes.TryGetValue(code, out mapped) ? mapped : -1;
        }

        /// Types text as Unicode key events, so any character arrives regardless of the host's layout.
        /// Line breaks and tabs are real Return and Tab presses, as on the other hosts.
        static void TypeText(string text)
        {
            for (int i = 0; i < text.Length; i++)
            {
                char c = text[i];
                if (c == '\r')
                {
                    if (i + 1 < text.Length && text[i + 1] == '\n') continue;
                    c = '\n';
                }
                if (c == '\n')
                {
                    Key(0x1C, true);
                    Key(0x1C, false);
                    continue;
                }
                if (c == '\t')
                {
                    Key(0x0F, true);
                    Key(0x0F, false);
                    continue;
                }
                foreach (bool down in new[] { true, false })
                {
                    var input = new Native.INPUT();
                    input.type = Native.INPUT_KEYBOARD;
                    input.u.ki.wScan = c;
                    input.u.ki.dwFlags = Native.KEYEVENTF_UNICODE | (down ? 0u : Native.KEYEVENTF_KEYUP);
                    Native.Send(input);
                }
            }
        }

        /// Puts the text on the host clipboard and presses Ctrl+V.
        static void Paste(string text)
        {
            ClipboardText.Write(text);
            Thread.Sleep(30);
            short packed = Native.VkKeyScan('v');
            int scan = packed != -1 ? (int)Native.MapVirtualKey((uint)(packed & 0xFF), 0) : 0;
            if (scan == 0) scan = 0x2F;
            Key(ScanCtrl, true);
            Key(scan, true);
            Key(scan, false);
            Key(ScanCtrl, false);
        }
    }

    // MARK: Clipboard

    static class ClipboardText
    {
        public static Dictionary<string, object> Read()
        {
            var result = new Dictionary<string, object>();
            result["changeCount"] = (long)Native.GetClipboardSequenceNumber();
            string text = Retry(delegate { return Clipboard.ContainsText() ? Clipboard.GetText() : null; });
            if (text != null) result["text"] = text;
            return result;
        }

        public static void Write(string text)
        {
            Retry(delegate
            {
                if (text.Length == 0) Clipboard.Clear();
                else Clipboard.SetText(text);
                return (string)null;
            });
        }

        /// Another app can hold the clipboard open for a moment; try a few times.
        static string Retry(Func<string> body)
        {
            for (int attempt = 0; ; attempt++)
            {
                try
                {
                    return body();
                }
                catch (ExternalException)
                {
                    if (attempt >= 5) throw new HelperError("the clipboard is in use by another app");
                    Thread.Sleep(30);
                }
            }
        }
    }

    // MARK: Win32

    static class Native
    {
        public const uint INPUT_MOUSE = 0, INPUT_KEYBOARD = 1;
        public const uint MOUSEEVENTF_MOVE = 0x0001, MOUSEEVENTF_LEFTDOWN = 0x0002, MOUSEEVENTF_LEFTUP = 0x0004;
        public const uint MOUSEEVENTF_RIGHTDOWN = 0x0008, MOUSEEVENTF_RIGHTUP = 0x0010;
        public const uint MOUSEEVENTF_MIDDLEDOWN = 0x0020, MOUSEEVENTF_MIDDLEUP = 0x0040;
        public const uint MOUSEEVENTF_WHEEL = 0x0800, MOUSEEVENTF_HWHEEL = 0x1000;
        public const uint MOUSEEVENTF_VIRTUALDESK = 0x4000, MOUSEEVENTF_ABSOLUTE = 0x8000;
        public const uint KEYEVENTF_EXTENDEDKEY = 0x0001, KEYEVENTF_KEYUP = 0x0002, KEYEVENTF_UNICODE = 0x0004, KEYEVENTF_SCANCODE = 0x0008;

        [StructLayout(LayoutKind.Sequential)]
        public struct POINT { public int x; public int y; }

        [StructLayout(LayoutKind.Sequential)]
        public struct RECT { public int left; public int top; public int right; public int bottom; }

        [StructLayout(LayoutKind.Sequential)]
        public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }

        [StructLayout(LayoutKind.Sequential)]
        public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }

        [StructLayout(LayoutKind.Explicit)]
        public struct InputUnion
        {
            [FieldOffset(0)] public MOUSEINPUT mi;
            [FieldOffset(0)] public KEYBDINPUT ki;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct INPUT { public uint type; public InputUnion u; }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct MONITORINFOEX
        {
            public int cbSize;
            public RECT rcMonitor;
            public RECT rcWork;
            public uint dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct DISPLAY_DEVICE
        {
            public int cb;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
            public int StateFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public POINT ptScreenPos; }

        [StructLayout(LayoutKind.Sequential)]
        public struct ICONINFO { public bool fIcon; public int xHotspot; public int yHotspot; public IntPtr hbmMask; public IntPtr hbmColor; }

        public delegate bool MonitorEnumProc(IntPtr monitor, IntPtr hdc, ref RECT rect, IntPtr data);

        [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc callback, IntPtr data);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFOEX info);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool EnumDisplayDevices(string device, uint index, ref DISPLAY_DEVICE info, uint flags);
        [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint count, INPUT[] inputs, int size);
        [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
        [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO info);
        [DllImport("user32.dll")] public static extern bool GetIconInfo(IntPtr icon, out ICONINFO info);
        [DllImport("user32.dll")] public static extern bool DrawIconEx(IntPtr hdc, int x, int y, IntPtr icon, int width, int height, int step, IntPtr brush, int flags);
        [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr handle);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern short VkKeyScan(char character);
        [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
        [DllImport("user32.dll")] public static extern uint GetClipboardSequenceNumber();
        [DllImport("user32.dll")] static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool GetUserObjectInformation(IntPtr handle, int index, StringBuilder info, int length, out int needed);
        [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desktop);
        [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window, out RECT rect);
        [DllImport("kernel32.dll")] static extern uint SetThreadExecutionState(uint flags);
        [DllImport("user32.dll")] static extern IntPtr SetProcessDpiAwarenessContext(IntPtr context);
        [DllImport("shcore.dll")] static extern int SetProcessDpiAwareness(int value);
        [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

        public static void Send(INPUT input)
        {
            SendInput(1, new[] { input }, Marshal.SizeOf(typeof(INPUT)));
        }

        /// Per-monitor DPI awareness makes monitor bounds, capture and input all use physical pixels.
        public static void EnableDpiAwareness()
        {
            // Each call exists from a later Windows version than the next; older ones fall through.
            try
            {
                if (SetProcessDpiAwarenessContext(new IntPtr(-4)) != IntPtr.Zero) return;
            }
            catch (DllNotFoundException) { }
            catch (EntryPointNotFoundException) { }
            try
            {
                if (SetProcessDpiAwareness(2) == 0) return;
            }
            catch (DllNotFoundException) { }
            catch (EntryPointNotFoundException) { }
            try
            {
                SetProcessDPIAware();
            }
            catch (DllNotFoundException) { }
        }

        /// True while the lock screen, sign-in screen or a UAC prompt (a secure desktop) has the input.
        public static bool SecureDesktopShowing()
        {
            IntPtr desktop = OpenInputDesktop(0, false, 0x0001);
            if (desktop == IntPtr.Zero) return true;
            try
            {
                var name = new StringBuilder(256);
                int needed;
                if (!GetUserObjectInformation(desktop, 2, name, name.Capacity * 2, out needed)) return false;
                return !string.Equals(name.ToString(), "Default", StringComparison.OrdinalIgnoreCase);
            }
            finally
            {
                CloseDesktop(desktop);
            }
        }

        public static Point ForegroundWindowCenter()
        {
            RECT rect;
            IntPtr window = GetForegroundWindow();
            if (window == IntPtr.Zero || !GetWindowRect(window, out rect)) return new Point(int.MinValue, int.MinValue);
            return new Point((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
        }

        /// Turns the display back on, as a key press would. Called only for an explicit wake request.
        public static void WakeDisplay()
        {
            SetThreadExecutionState(0x00000002); // ES_DISPLAY_REQUIRED, once: resets the display idle timer.
            var nudge = new INPUT();
            nudge.type = INPUT_MOUSE;
            nudge.u.mi.dwFlags = MOUSEEVENTF_MOVE;
            nudge.u.mi.dx = 1;
            Send(nudge);
            nudge.u.mi.dx = -1;
            Send(nudge);
        }
    }
}
