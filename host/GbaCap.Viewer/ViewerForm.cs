using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using LibUsbDotNet;
using LibUsbDotNet.LibUsb;
using LibUsbDotNet.Main;

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
    private int _scale = 3;
    private int _framesThisSec;
    private string _status = "接続中...";

    private bool _fullscreen;
    private ToolStripMenuItem? _viewMenu;
    private string _flash = ""; // タイトルバーへの一時メッセージ

    // Windows 11 の角丸を無効化（映像領域の四隅が欠けるのを防ぐ）
    [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    public ViewerForm()
    {
        Text = "GBAcapture";
        DoubleBuffered = true;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = true; // □は下のWndProcで 1x として扱う
        BackColor = Color.Black;
        KeyPreview = true;

        var menu = new MenuStrip();
        var view = new ToolStripMenuItem("表示(&V)");
        _viewMenu = view;
        foreach (var s in new[] { 1, 2, 3, 4 })
        {
            var item = new ToolStripMenuItem($"{s}x") { Checked = s == _scale, Tag = s };
            item.Click += (_, _) => SetScale((int)item.Tag!, view);
            view.DropDownItems.Add(item);
        }
        view.DropDownItems.Add(new ToolStripSeparator());
        var fsItem = new ToolStripMenuItem("フルスクリーン (Escで戻る)");
        fsItem.Click += (_, _) => EnterFullscreen();
        view.DropDownItems.Add(fsItem);
        view.DropDownItems.Add(new ToolStripSeparator());
        var ssItem = new ToolStripMenuItem("スクリーンショット保存 (F12)");
        ssItem.Click += (_, _) => SaveScreenshot();
        view.DropDownItems.Add(ssItem);
        menu.Items.Add(view);
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
                Text = $"GBAcapture - {_framesThisSec} fps ({_scale}x) {_status}{_flash}";
            _framesThisSec = 0;
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
            _flash = $"  [保存: {path}]";
            var t = new System.Windows.Forms.Timer { Interval = 4000 };
            t.Tick += (_, _) => { _flash = ""; t.Dispose(); };
            t.Start();
        }
        catch (Exception ex)
        {
            _flash = $"  [保存失敗: {ex.Message}]";
        }
    }

    // □(最大化ボタン) は 1x に、画面上端へのスナップ(Aeroスナップ)はフルスクリーンにする
    protected override void WndProc(ref Message m)
    {
        const int WM_SYSCOMMAND = 0x0112;
        const int SC_MAXIMIZE = 0xF030;
        if (m.Msg == WM_SYSCOMMAND && ((int)m.WParam & 0xFFF0) == SC_MAXIMIZE)
        {
            // マウスが画面最上端ならスナップ操作 → フルスクリーン
            if (MousePosition.Y <= 8)
                EnterFullscreen();
            else if (_viewMenu is not null)
                SetScale(1, _viewMenu);
            return; // 既定の最大化はさせない
        }
        base.WndProc(ref m);
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
        foreach (ToolStripMenuItem it in parent.DropDownItems)
            it.Checked = (int)it.Tag! == s;
        ApplyClientSize();
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
                _status = "デバイス未接続";
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
            _running = true;
            SendCmd('S');
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
            _status = $"接続失敗: {ex.Message}";
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
                    if (!(buf[pos] == 0xA5 && buf[pos + 3] == 0 && buf[pos + 2] < 160))
                    {
                        pos++; // マジック再同期
                        continue;
                    }
                    var line = buf[pos + 2];
                    DecodeRow(buf, pos, row);
                    WriteRow(line, row);
                    if (line == H - 1)
                    {
                        _framesThisSec++;
                        BeginInvoke(Invalidate);
                    }
                    pos += LinePacket.PacketSize;
                }
                // 未処理の端数を先頭へ詰める
                Buffer.BlockCopy(buf, pos, buf, 0, len - pos);
                len -= pos;
            }
        }
        catch (Exception ex)
        {
            _status = "受信停止";
            try { File.AppendAllText("viewer.log", $"[{epId}] {ex}\n"); } catch { }
        }
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
        }
        else
        {
            var menuH = MainMenuStrip?.Height ?? 24;
            lock (_bmpLock)
                e.Graphics.DrawImage(_bmp, 0, menuH, W * _scale, H * _scale);
        }
    }
}
