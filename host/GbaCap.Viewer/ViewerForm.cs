using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using LibUsbDotNet;
using LibUsbDotNet.LibUsb;
using LibUsbDotNet.Main;
using NAudio.CoreAudioApi;
using NAudio.Wave;

namespace GbaCap.Viewer;

/// <summary>
/// GBAcapture リアルタイムビューア。
/// FWのストリーミングモード('S')でライン即時送出させ、240x160をピクセルパーフェクトに
/// 整数倍率(1x-8x)で表示する。録画はOBSのウィンドウキャプチャで行う想定。
/// </summary>
public sealed class ViewerForm : Form
{
    private const int Vid = 0x1209;
    private const int Pid = 0xC001;
    private const int W = 240;
    private const int H = 160;

    private readonly Bitmap _bmp = new(W, H, PixelFormat.Format32bppRgb);
    private readonly object _bmpLock = new();
    private readonly System.Windows.Forms.Timer _fpsTimer = new() { Interval = 1000 };

    private UsbContext? _ctx;
    private IUsbDevice? _dev;
    private readonly List<Thread> _rxThreads = new();
    private volatile bool _running;
    private static readonly ReadEndpointID[] StreamEps =
        [ReadEndpointID.Ep04, ReadEndpointID.Ep01, ReadEndpointID.Ep03];
    private int _scale = 1; // 既定1x。前回終了時の値を設定ファイルから復元する
    private int _volumePercent = 50;
    private int _framesThisSec;
    private long _lastFrameMs; // 最後にフレームが完成した時刻 (NO SIGNAL 表示用)
    private string _status = "Connecting...";
    private bool _dragging;   // タイトルバードラッグ中 (WM_ENTER/EXITSIZEMOVE)

    private static string SettingsPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "GBAcapture", "viewer.json");

    private void LoadSettings()
    {
        try
        {
            var json = System.Text.Json.JsonDocument.Parse(File.ReadAllText(SettingsPath));
            if (json.RootElement.TryGetProperty("scale", out var sc)) _scale = Math.Clamp(sc.GetInt32(), 1, 4);
            if (json.RootElement.TryGetProperty("volume", out var vo)) _volumePercent = Math.Clamp(vo.GetInt32(), 0, 100);
        }
        catch { /* 初回起動など */ }
        _volume = _volumePercent / 100f;
    }

    private void SaveSettings()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath)!);
            File.WriteAllText(SettingsPath,
                $"{{\"scale\":{_scale},\"volume\":{_volumePercent}}}");
        }
        catch { }
    }

    private bool _fullscreen;
    private ToolStripMenuItem? _viewMenu;
    private string _flash = ""; // タイトルバーへの一時メッセージ

    // 音声 (パケット0xA6: 32.8kHz 12bit生 → int16 PCM → 48kHzリサンプル)
    private BufferedWaveProvider? _audioBuf;
    private WasapiOut? _audioOut;
    private IWaveProvider? _audioChain;      // リサンプル済みの再生チェーン
    private MMDeviceEnumerator? _mmEnum;     // 既定出力デバイス変更の監視
    private AudioDeviceWatcher? _mmWatcher;
    private readonly object _audioOutLock = new();

    // 既定出力デバイスが変わったら WasapiOut を作り直して追従する
    private sealed class AudioDeviceWatcher : NAudio.CoreAudioApi.Interfaces.IMMNotificationClient
    {
        private readonly Action _onChanged;
        public AudioDeviceWatcher(Action onChanged) => _onChanged = onChanged;
        public void OnDefaultDeviceChanged(DataFlow flow, Role role, string defaultDeviceId)
        {
            if (flow == DataFlow.Render && role == Role.Multimedia) _onChanged();
        }
        public void OnDeviceAdded(string pwstrDeviceId) { }
        public void OnDeviceRemoved(string deviceId) { }
        public void OnDeviceStateChanged(string deviceId, DeviceState newState) { }
        public void OnPropertyValueChanged(string pwstrDeviceId, NAudio.CoreAudioApi.PropertyKey key) { }
    }

    private string? _currentAudioDevId;

    private string? DefaultRenderId()
    {
        try { return _mmEnum?.GetDefaultAudioEndpoint(DataFlow.Render, Role.Multimedia)?.ID; }
        catch { return null; }
    }

    private void RestartAudioOut()
    {
        lock (_audioOutLock)
        {
            if (_audioChain is null) return;
            try { _audioOut?.Stop(); _audioOut?.Dispose(); } catch { }
            try
            {
                _audioOut = new WasapiOut(AudioClientShareMode.Shared, 120); // 新しい既定デバイスで開き直す
                _audioOut.Init(_audioChain);
                _audioOut.Play();
                _currentAudioDevId = DefaultRenderId();
            }
            catch { _audioOut = null; }
        }
    }
    private volatile bool _mute;
    private volatile float _volume = 0.5f;
    private float _dcL = 2048, _dcR = 2048; // DCオフセット追従（ポップ音防止）

    // デバッグ録音 (生サンプルをWAV保存 → スペクトル解析用)
    private WaveFileWriter? _dbgWav;
    private int _dbgRemain;
    private readonly object _dbgLock = new();

    private void StartDebugRecording(int seconds)
    {
        lock (_dbgLock)
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.MyPictures), "GBAcapture");
            Directory.CreateDirectory(dir);
            var path = Path.Combine(dir, $"audio_debug_{DateTime.Now:yyyyMMdd_HHmmss}.wav");
            _dbgWav = new WaveFileWriter(path, new WaveFormat(32802, 16, 2));
            _dbgRemain = 32802 * seconds;
            _flash = $"  [Recording: {path}]";
        }
    }

    // Windows 11 の角丸を無効化（映像領域の四隅が欠けるのを防ぐ）
    [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    public ViewerForm()
    {
        LoadSettings();
        Text = "GBAcapture (Nisekoro-capture)";
        DoubleBuffered = true;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = true; // □は下のWndProcで 1x として扱う
        BackColor = Color.Black;
        KeyPreview = true;

        var menu = new MenuStrip();
        var view = new ToolStripMenuItem("&View");
        _viewMenu = view;
        foreach (var s in new[] { 1, 2, 3, 4 })
        {
            var item = new ToolStripMenuItem($"{s}x") { Checked = s == _scale, Tag = s };
            item.Click += (_, _) => SetScale((int)item.Tag!, view);
            view.DropDownItems.Add(item);
        }
        view.DropDownItems.Add(new ToolStripSeparator());
        var fsItem = new ToolStripMenuItem("Fullscreen (Esc to exit)");
        fsItem.Click += (_, _) => EnterFullscreen();
        view.DropDownItems.Add(fsItem);
        view.DropDownItems.Add(new ToolStripSeparator());
        var ssItem = new ToolStripMenuItem("Save Screenshot (F12)");
        ssItem.Click += (_, _) => SaveScreenshot();
        view.DropDownItems.Add(ssItem);
        var audio = new ToolStripMenuItem("&Audio");
        var muteItem = new ToolStripMenuItem("Mute") { CheckOnClick = true };
        audio.DropDownItems.Add(muteItem);
        audio.DropDownItems.Add(new ToolStripSeparator());

        // 音量: プリセット + スライダー
        var volSlider = new TrackBar
        {
            Minimum = 0, Maximum = 100, Value = _volumePercent,
            TickFrequency = 25, Width = 160, AutoSize = false, Height = 28,
        };
        var presetItems = new List<ToolStripMenuItem>();
        void SetVolume(int percent)
        {
            _volume = percent / 100f;
            _volumePercent = percent;
            if (muteItem.Checked) muteItem.Checked = false; // 音量操作でミュート解除
            if (volSlider.Value != percent) volSlider.Value = percent;
            foreach (var pi in presetItems)
                pi.Checked = (int)pi.Tag! == percent;
            SaveSettings();
        }
        // Mute と Volume は排他: ミュート中は音量チェックを外し、解除で復元
        muteItem.CheckedChanged += (_, _) =>
        {
            _mute = muteItem.Checked;
            foreach (var pi in presetItems)
                pi.Checked = !_mute && (int)pi.Tag! == _volumePercent;
        };
        foreach (var p in new[] { 25, 50, 75, 100 })
        {
            var item = new ToolStripMenuItem($"Volume {p}%") { Tag = p, Checked = p == _volumePercent };
            item.Click += (_, _) => SetVolume((int)item.Tag!);
            presetItems.Add(item);
            audio.DropDownItems.Add(item);
        }
        volSlider.ValueChanged += (_, _) => SetVolume(volSlider.Value);
        audio.DropDownItems.Add(new ToolStripControlHost(volSlider));
        menu.Items.Add(view);
        menu.Items.Add(audio);
        MainMenuStrip = menu;
        Controls.Add(menu);

        KeyDown += (_, e) =>
        {
            if (e.KeyCode == Keys.Escape && _fullscreen) ExitFullscreen();
            if (e.KeyCode == Keys.F12) SaveScreenshot();
        };

        ApplyClientSize();
        _fpsTimer.Tick += (_, _) =>
        {
            if (!_fullscreen)
                Text = $"GBAcapture (Nisekoro-capture) - {_framesThisSec} fps ({_scale}x) {_status}{_flash}";
            _framesThisSec = 0;
            if (Environment.TickCount64 - _lastFrameMs > 1500)
                Invalidate(); // NO SIGNAL オーバーレイ更新
        };
        _fpsTimer.Start();
    }

    private void SaveScreenshot()
    {
        try
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.MyPictures), "GBAcapture");
            Directory.CreateDirectory(dir);
            var path = Path.Combine(dir, $"gbacap_{DateTime.Now:yyyyMMdd_HHmmss_fff}.png");
            lock (_bmpLock)
            {
                using var clone = new Bitmap(_bmp);
                clone.Save(path, ImageFormat.Png);
            }
            _flash = $"  [Saved: {path}]";
            var t = new System.Windows.Forms.Timer { Interval = 4000 };
            t.Tick += (_, _) => { _flash = ""; t.Dispose(); };
            t.Start();
        }
        catch (Exception ex)
        {
            _flash = $"  [Save failed: {ex.Message}]";
        }
    }

    // ドラッグで画面上端にぶつけて離す→フルスクリーン / □ボタンクリック→1x
    // 注意: ウィンドウスタイル変更はドラッグのモーダルループ内で行うと表示が壊れるため、
    // 必ず BeginInvoke でループ脱出後に実行する
    protected override void WndProc(ref Message m)
    {
        const int WM_SYSCOMMAND = 0x0112;
        const int SC_MAXIMIZE = 0xF030;
        const int WM_ENTERSIZEMOVE = 0x0231;
        const int WM_EXITSIZEMOVE = 0x0232;
        if (m.Msg == WM_ENTERSIZEMOVE) _dragging = true;
        if (m.Msg == WM_EXITSIZEMOVE)
        {
            _dragging = false;
            if (!_fullscreen)
            {
                if (MousePosition.Y <= 6)
                    BeginInvoke(EnterFullscreen);       // 上端で離した → フルスクリーン
                else
                    BeginInvoke(ApplyClientSize);        // スナップ等で崩れたサイズを自己修復
            }
        }
        if (m.Msg == WM_SYSCOMMAND && ((int)m.WParam & 0xFFF0) == SC_MAXIMIZE)
        {
            if (_dragging || MousePosition.Y <= 8)
                BeginInvoke(EnterFullscreen);
            else if (_viewMenu is not null)
            {
                var vm = _viewMenu;
                BeginInvoke(() => SetScale(1, vm));
            }
            return; // 既定の最大化はさせない
        }
        base.WndProc(ref m);
    }

    // フルスクリーン中に上部をつまんでドラッグ → 通常サイズに戻して掴んだまま移動
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool ReleaseCapture();
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    private bool _fsGrab;
    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (_fullscreen && e.Button == MouseButtons.Left && e.Y < 48)
            _fsGrab = true;
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (_fsGrab && e.Button == MouseButtons.Left)
        {
            _fsGrab = false;
            ExitFullscreen();
            // 掴んだままウィンドウ移動を継続 (通常のタイトルバードラッグへ移譲)
            ReleaseCapture();
            SendMessage(Handle, 0x00A1 /*WM_NCLBUTTONDOWN*/, (IntPtr)2 /*HTCAPTION*/, IntPtr.Zero);
        }
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        _fsGrab = false;
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        var pref = 1; // DWMWCP_DONOTROUND
        try { DwmSetWindowAttribute(Handle, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, ref pref, 4); }
        catch { }
    }

    private void EnterFullscreen()
    {
        _fullscreen = true;
        MainMenuStrip!.Visible = false;
        if (WindowState == FormWindowState.Maximized)
            WindowState = FormWindowState.Normal; // 一旦戻さないと borderless 最大化が反映されない
        FormBorderStyle = FormBorderStyle.None;
        WindowState = FormWindowState.Maximized;
        Invalidate();
    }

    private void ExitFullscreen()
    {
        _fullscreen = false;
        WindowState = FormWindowState.Normal;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MainMenuStrip!.Visible = true;
        ApplyClientSize();
        Invalidate();
    }

    private void SetScale(int s, ToolStripMenuItem parent)
    {
        _scale = s;
        // セパレータ等が混ざるので ToolStripMenuItem だけを対象にする
        foreach (var it in parent.DropDownItems.OfType<ToolStripMenuItem>())
            it.Checked = it.Tag is int v && v == s;
        if (_fullscreen)
            ExitFullscreen();
        ApplyClientSize();
        SaveSettings();
        Invalidate();
    }

    private void ApplyClientSize()
    {
        var menuH = MainMenuStrip?.Height ?? 24;
        ClientSize = new Size(W * _scale, H * _scale + menuH);
    }

    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);
        try
        {
            _ctx = new UsbContext();
            _dev = _ctx.Find(d => d.VendorId == Vid && d.ProductId == Pid);
            if (_dev is null)
            {
                _status = "Device not found";
                return;
            }
            _dev.Open();
            _dev.ClaimInterface(0);

            // 残骸排水 → ストリーミング開始 → EPごとの受信スレッド起動
            SendCmd('X');
            foreach (var ep in StreamEps)
            {
                var rd = _dev.OpenEndpointReader(ep);
                var drain = new byte[16384];
                while (rd.Read(drain, 100, out var n) == Error.Success && n > 0) { }
            }
            // 音声出力: FWから32.768kHz(PWMキャリア同期)で届くのでWDLリサンプラで48kHz化
            try
            {
                _audioBuf = new BufferedWaveProvider(new WaveFormat(32802, 16, 2)) // FW実レート(328.02k/10)に一致させドリフトを防ぐ
                {
                    BufferDuration = TimeSpan.FromSeconds(2),
                    DiscardOnBufferOverflow = true,
                };
                var resampled = new NAudio.Wave.SampleProviders.WdlResamplingSampleProvider(
                    _audioBuf.ToSampleProvider(), 48000);
                _audioChain = resampled.ToWaveProvider16();
                _audioOut = new WasapiOut(AudioClientShareMode.Shared, 120);
                _audioOut.Init(_audioChain);
                _audioOut.Play();

                // Windowsの既定出力デバイス変更に即時追従
                _mmEnum = new MMDeviceEnumerator();
                _currentAudioDevId = DefaultRenderId();
                var kick = new System.Windows.Forms.Timer { Interval = 50 };
                kick.Tick += (_, _) => { kick.Stop(); RestartAudioOut(); };
                _mmWatcher = new AudioDeviceWatcher(() =>
                {
                    try { BeginInvoke(() => { kick.Stop(); kick.Start(); }); } catch { }
                });
                try { _mmEnum.RegisterEndpointNotificationCallback(_mmWatcher); } catch { }
                // フォールバック: 通知が来ない環境向けに500msごとに既定デバイスIDを照合
                var poll = new System.Windows.Forms.Timer { Interval = 500 };
                poll.Tick += (_, _) =>
                {
                    var id = DefaultRenderId();
                    if (id is not null && id != _currentAudioDevId)
                        RestartAudioOut();
                };
                poll.Start();
            }
            catch { /* 音声デバイスが無くても映像は動かす */ }

            _running = true;
            SendCmd('S');
            // 環境変数指定時は起動直後から自動デバッグ録音（解析用）
            if (Environment.GetEnvironmentVariable("GBACAP_AUDIODUMP") is not null)
                StartDebugRecording(10);
            foreach (var ep in StreamEps)
            {
                var t = new Thread(() => RxLoop(ep)) { IsBackground = true };
                t.Start();
                _rxThreads.Add(t);
            }
            _status = "";
        }
        catch (Exception ex)
        {
            _status = $"Connect failed: {ex.Message}";
        }
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        _running = false;
        foreach (var t in _rxThreads) t.Join(1000);
        try
        {
            if (_dev is not null)
            {
                SendCmd('X');
                var drain = new byte[16384];
                foreach (var ep in StreamEps)
                {
                    var rd = _dev.OpenEndpointReader(ep);
                    while (rd.Read(drain, 100, out var n) == Error.Success && n > 0) { }
                }
            }
        }
        catch { }
        try
        {
            if (_mmEnum is not null && _mmWatcher is not null)
                _mmEnum.UnregisterEndpointNotificationCallback(_mmWatcher);
        }
        catch { }
        lock (_audioOutLock)
        {
            try { _audioOut?.Stop(); _audioOut?.Dispose(); } catch { }
        }
        _dev?.Dispose();
        _ctx?.Dispose();
        base.OnFormClosing(e);
    }

    private void SendCmd(char c)
    {
        var w = _dev!.OpenEndpointWriter(WriteEndpointID.Ep05);
        byte[] pkt = [(byte)'G', (byte)'B', (byte)'A', (byte)'C', (byte)c];
        w.Write(pkt, 1000, out _);
    }

    private void RxLoop(ReadEndpointID epId)
    {
        try
        {
            var reader = _dev!.OpenEndpointReader(epId);
            var chunk = new byte[65536];
            var buf = new byte[LinePacket.PacketSize * 64];
            var len = 0;
            var row = new int[W];

            while (_running)
            {
                var ec = reader.Read(buf, len, Math.Min(chunk.Length, buf.Length - len), 200, out var n);
                if (ec == Error.Timeout) continue;
                if (ec != Error.Success)
                {
                    try { File.AppendAllText("viewer.log", $"[{epId}] read error: {ec}\n"); } catch { }
                    break;
                }
                len += n;

                var pos = 0;
                while (len - pos >= LinePacket.PacketSize)
                {
                    if (buf[pos] == 0xA5 && buf[pos + 3] == 0 && buf[pos + 2] < 160)
                    {
                        var line = buf[pos + 2];
                        DecodeRow(buf, pos, row);
                        WriteRow(line, row);
                        if (line == H - 1)
                        {
                            _framesThisSec++;
                            _lastFrameMs = Environment.TickCount64;
                            BeginInvoke(Invalidate);
                        }
                        pos += LinePacket.PacketSize;
                    }
                    else if (buf[pos] == 0xA6 && buf[pos + 2] > 0 && buf[pos + 2] <= 120 && buf[pos + 3] == 0)
                    {
                        PlayAudioPacket(buf, pos, buf[pos + 2]);
                        pos += LinePacket.PacketSize;
                    }
                    else
                    {
                        pos++; // マジック再同期
                    }
                }
                // 未処理の端数を先頭へ詰める
                Buffer.BlockCopy(buf, pos, buf, 0, len - pos);
                len -= pos;
            }
        }
        catch (Exception ex)
        {
            _status = "RX stopped";
            try { File.AppendAllText("viewer.log", $"[{epId}] {ex}\n"); } catch { }
        }
    }

    // 2次バターワースLPF fc=13kHz @32.768kHz (残留ノイズの安全網)
    private sealed class Biquad
    {
        private const float B0 = 0.6309f, B1 = 1.2618f, B2 = 0.6309f, A1 = 1.1206f, A2 = 0.4030f;
        private float _x1, _x2, _y1, _y2;
        public float Process(float x)
        {
            var y = B0 * x + B1 * _x1 + B2 * _x2 - A1 * _y1 - A2 * _y2;
            _x2 = _x1; _x1 = x;
            _y2 = _y1; _y1 = y;
            return y;
        }
    }

    private readonly Biquad _lpfL = new(), _lpfR = new();

    // 音声パケット: [0xA6][seq][ペア数][0] + 32.768kHz L,R交互のADC生12bit
    private void PlayAudioPacket(byte[] buf, int pos, int pairs)
    {
        // デバッグ録音: 加工前の生サンプルを保存
        if (_dbgWav is not null)
        {
            lock (_dbgLock)
            {
                if (_dbgWav is not null)
                {
                    for (var j = 0; j < pairs && _dbgRemain > 0; j++, _dbgRemain--)
                    {
                        int rl = buf[pos + 4 + j * 4] | (buf[pos + 5 + j * 4] << 8);
                        int rr = buf[pos + 6 + j * 4] | (buf[pos + 7 + j * 4] << 8);
                        _dbgWav.WriteSample((rl - 2048) / 2048f);
                        _dbgWav.WriteSample((rr - 2048) / 2048f);
                    }
                    if (_dbgRemain <= 0)
                    {
                        _dbgWav.Dispose();
                        _dbgWav = null;
                        _flash = "  [Recording done]";
                    }
                }
            }
        }

        if (_audioBuf is null || _mute) return;
        var pcm = new byte[pairs * 4];
        for (var j = 0; j < pairs; j++)
        {
            // パケット内の並びは [SO1, SO2]。pantest ROM での実測により SO1=右, SO2=左 と確定
            int r = buf[pos + 4 + j * 4] | (buf[pos + 5 + j * 4] << 8);
            int l = buf[pos + 6 + j * 4] | (buf[pos + 7 + j * 4] << 8);
            // DCオフセットをゆっくり追従して除去 → LPF → 音量 → int16 (12bit→16bit)
            _dcL += (l - _dcL) * 0.0005f;
            _dcR += (r - _dcR) * 0.0005f;
            var fl = _lpfL.Process(l - _dcL);
            var fr = _lpfR.Process(r - _dcR);
            var sl = Math.Clamp((int)(fl * 2 * _volume), short.MinValue, short.MaxValue); // 100%で常識的音量になる基準ゲイン
            var sr = Math.Clamp((int)(fr * 2 * _volume), short.MinValue, short.MaxValue);
            pcm[j * 4] = (byte)sl; pcm[j * 4 + 1] = (byte)(sl >> 8);
            pcm[j * 4 + 2] = (byte)sr; pcm[j * 4 + 3] = (byte)(sr >> 8);
        }
        _audioBuf.AddSamples(pcm, 0, pcm.Length);
    }

    private static void DecodeRow(byte[] buf, int pos, int[] row)
    {
        // v0.20 ストリーミングはライン境界が正確（VBLANK休止アンカー方式）なのでオフセット不要
        for (var x = 0; x < W; x++)
        {
            var b = (ushort)(buf[pos + 4 + x * 2] | (buf[pos + 5 + x * 2] << 8));
            var (r, g, bl) = PixelDecoder.DecodeSample(b);
            row[x] = (r << 16) | (g << 8) | bl;
        }
    }

    private void WriteRow(int line, int[] row)
    {
        lock (_bmpLock)
        {
            var d = _bmp.LockBits(new Rectangle(0, line, W, 1), ImageLockMode.WriteOnly,
                PixelFormat.Format32bppRgb);
            System.Runtime.InteropServices.Marshal.Copy(row, 0, d.Scan0, W);
            _bmp.UnlockBits(d);
        }
    }

    // 映像が一定時間来ていないときのオーバーレイ (GBA電源断・ケーブル抜け等)
    private void DrawNoSignal(Graphics g, Rectangle area)
    {
        if (Environment.TickCount64 - _lastFrameMs <= 1500) return;
        using var dim = new SolidBrush(Color.FromArgb(140, 0, 0, 0));
        g.FillRectangle(dim, area);
        using var font = new Font("Segoe UI", Math.Max(10, area.Height / 12f), FontStyle.Bold);
        const string msg = "NO SIGNAL";
        var size = g.MeasureString(msg, font);
        var pt = new PointF(area.X + (area.Width - size.Width) / 2,
                            area.Y + (area.Height - size.Height) / 2);
        using var shadow = new SolidBrush(Color.FromArgb(180, 0, 0, 0));
        g.DrawString(msg, font, shadow, pt.X + 2, pt.Y + 2);
        g.DrawString(msg, font, Brushes.White, pt);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.InterpolationMode = InterpolationMode.NearestNeighbor;
        e.Graphics.PixelOffsetMode = PixelOffsetMode.Half;
        if (_fullscreen)
        {
            // アスペクト比(3:2)維持で最大化。左右(または上下)は黒帯
            var cw = ClientSize.Width;
            var ch = ClientSize.Height;
            var fit = Math.Min((float)cw / W, (float)ch / H);
            var dw = (int)(W * fit);
            var dh = (int)(H * fit);
            lock (_bmpLock)
                e.Graphics.DrawImage(_bmp, (cw - dw) / 2, (ch - dh) / 2, dw, dh);
            DrawNoSignal(e.Graphics, new Rectangle((cw - dw) / 2, (ch - dh) / 2, dw, dh));
        }
        else
        {
            var menuH = MainMenuStrip?.Height ?? 24;
            lock (_bmpLock)
                e.Graphics.DrawImage(_bmp, 0, menuH, W * _scale, H * _scale);
            DrawNoSignal(e.Graphics, new Rectangle(0, menuH, W * _scale, H * _scale));
        }
    }
}
