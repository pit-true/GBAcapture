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

    public ViewerForm()
    {
        Text = "GBAcapture";
        DoubleBuffered = true;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;

        var menu = new MenuStrip();
        var view = new ToolStripMenuItem("表示(&V)");
        foreach (var s in new[] { 1, 2, 3, 4, 8 })
        {
            var item = new ToolStripMenuItem($"{s}x") { Checked = s == _scale, Tag = s };
            item.Click += (_, _) => SetScale((int)item.Tag!, view);
            view.DropDownItems.Add(item);
        }
        menu.Items.Add(view);
        MainMenuStrip = menu;
        Controls.Add(menu);

        ApplyClientSize();
        _fpsTimer.Tick += (_, _) =>
        {
            Text = $"GBAcapture - {_framesThisSec} fps ({_scale}x) {_status}";
            _framesThisSec = 0;
        };
        _fpsTimer.Start();
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
        var menuH = MainMenuStrip?.Height ?? 24;
        e.Graphics.InterpolationMode = InterpolationMode.NearestNeighbor;
        e.Graphics.PixelOffsetMode = PixelOffsetMode.Half;
        lock (_bmpLock)
            e.Graphics.DrawImage(_bmp, 0, menuH, W * _scale, H * _scale);
    }
}
