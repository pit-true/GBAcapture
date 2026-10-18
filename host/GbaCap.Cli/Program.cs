using GbaCap;
using LibUsbDotNet;
using LibUsbDotNet.LibUsb;
using LibUsbDotNet.Main;

// GBAcapture デバイス操作 CLI
//   ping              : 疎通確認（モード=APP/IAP とバージョン表示）
//   freq              : GBA信号の実測周波数を表示 (M1)
//   update <app.bin>  : 手放しFW更新（APP→IAP遷移→消去→書込→検証→再起動）
//   cmd <X>           : 任意コマンド送信+応答ダンプ（診断用）
const int Vid = 0x1209;
const int Pid = 0xC001;

if (args.Length < 1)
{
    Console.Error.WriteLine("usage: gbacap-cli <ping|freq|update <bin>|cmd <X>>");
    return 2;
}

using var context = new UsbContext();

switch (args[0])
{
    case "ping":
    {
        using var dev = Open(context);
        if (dev is null) return 1;
        var (mode, ver) = Ping(dev);
        Console.WriteLine(mode == 0xAA ? $"PONG: IAP v{ver} (更新モード)" : $"PONG: APP v0.{ver}");
        return 0;
    }
    case "freq":
    {
        using var dev = Open(context);
        if (dev is null) return 1;
        var buf = Transact(dev, MakeCmd('F'), 20);
        if (buf[4] != 'F') { Console.Error.WriteLine("freq 失敗"); return 1; }
        var dck = BitConverter.ToUInt32(buf, 8);
        var lp = BitConverter.ToUInt32(buf, 12);
        var sps = BitConverter.ToUInt32(buf, 16);
        Console.WriteLine($"DCK: {dck,10} /s (期待: 2293632 = 38400×59.73)");
        Console.WriteLine($"LP : {lp,10} /s (期待: 13618 ≒ 4194304/308)");
        Console.WriteLine($"SPS: {sps,10} /s (期待: 59.73 → 59 or 60)");
        if (sps > 0)
            Console.WriteLine($"DCK/フレーム: {(double)dck / sps:F0}, LP/フレーム: {(double)lp / sps:F1}");
        return 0;
    }
    case "update":
    {
        if (args.Length < 2) { Console.Error.WriteLine("usage: update <app.bin>"); return 2; }
        var image = File.ReadAllBytes(args[1]);
        if (image.Length % 2 == 1) image = [.. image, (byte)0xFF];
        Console.WriteLine($"イメージ: {args[1]} ({image.Length} bytes)");

        // 1. APPモードなら IAP へ遷移させる
        {
            using var dev = Open(context);
            if (dev is null) return 1;
            var (mode, _) = Ping(dev);
            if (mode != 0xAA)
            {
                Console.WriteLine("APP動作中 → IAP更新モードへ遷移させます...");
                Write(dev, MakeCmd('U'));
            }
        }

        // 2. 再列挙を待って IAP モードで開き直す
        using var iap = Reopen(context, expectIapMode: true, timeoutMs: 15000);
        if (iap is null) { Console.Error.WriteLine("IAPモードで再接続できませんでした"); return 1; }
        Console.WriteLine("IAP更新モードに入りました");

        // 3. 消去
        var e = Transact(iap, MakeCmd('E', sizeLE: (uint)image.Length), 8, timeoutMs: 30000);
        if (e[4] != 'e' || e[5] != 0) { Console.Error.WriteLine($"消去失敗 status={e[5]}"); return 1; }
        Console.WriteLine($"消去完了 ({e[6]} ページ)");

        // 4. 書き込み (256B チャンク)
        for (var off = 0; off < image.Length; off += 256)
        {
            var len = Math.Min(256, image.Length - off);
            var pkt = new byte[12 + len];
            "GBAC"u8.CopyTo(pkt);
            pkt[4] = (byte)'W';
            BitConverter.GetBytes((ushort)len).CopyTo(pkt, 6);
            BitConverter.GetBytes((uint)off).CopyTo(pkt, 8);
            image.AsSpan(off, len).CopyTo(pkt.AsSpan(12));
            var w = Transact(iap, pkt, 8, timeoutMs: 5000);
            if (w[4] != 'w' || w[5] != 0) { Console.Error.WriteLine($"書込失敗 @0x{off:X} status={w[5]}"); return 1; }
            if (off % 4096 == 0) Console.Write($"\r書き込み中... {off}/{image.Length}");
        }
        Console.WriteLine($"\r書き込み完了 {image.Length}/{image.Length}");

        // 5. 検証（加算チェックサム）
        var v = Transact(iap, MakeCmd('V', sizeLE: (uint)image.Length), 12, timeoutMs: 10000);
        var devSum = BitConverter.ToUInt32(v, 8);
        uint localSum = 0;
        foreach (var b in image) localSum += b;
        if (devSum != localSum)
        {
            Console.Error.WriteLine($"検証失敗: device={devSum:X8} local={localSum:X8}");
            return 1;
        }
        Console.WriteLine($"検証OK (sum={devSum:X8})");

        // 6. 再起動して APP へ
        Write(iap, MakeCmd('G'));
        iap.Dispose();
        using var app = Reopen(context, expectIapMode: false, timeoutMs: 15000);
        if (app is null) { Console.Error.WriteLine("APPの再起動を確認できませんでした"); return 1; }
        var (m2, ver2) = Ping(app);
        Console.WriteLine($"更新完了! APP v0.{ver2} が起動しました");
        return 0;
    }
    case "capture": // 1フレーム(240x160)を32ラインずつ取得してBMP保存
    {
        var outPath = args.Length >= 2 ? args[1] : "capture.bmp";
        using var dev = Open(context);
        if (dev is null) return 1;
        var reader = dev.OpenEndpointReader(ReadEndpointID.Ep04);
        var lines = new Dictionary<int, LinePacket>();

        // 前回実行の残骸がパイプに残っていることがあるため排水する
        {
            var drain = new byte[4096];
            while (reader.Read(drain, 100, out var dn) == Error.Success && dn > 0) { }
        }

        for (var start = 0; start < 160; start += 32)
        {
            var cmd = new byte[12];
            "GBAC"u8.CopyTo(cmd);
            cmd[4] = (byte)'L';
            BitConverter.GetBytes((ushort)start).CopyTo(cmd, 6);
            BitConverter.GetBytes((ushort)32).CopyTo(cmd, 8);
            Write(dev, cmd);

            // バルクはバイトストリーム。グループ全体 (32×724B) を読み切ってから分割する
            var total = 32 * LinePacket.PacketSize;
            var buf = new byte[total];
            var got = 0;
            while (got < total)
            {
                var ec = reader.Read(buf, got, total - got, 3000, out var n);
                if (ec != Error.Success || n == 0)
                {
                    if (got == 8 && buf[4] == 'l')
                    {
                        Console.Error.WriteLine(
                            $"キャプチャ失敗 status={buf[5]} filled={buf[6]} line={buf[7]}" +
                            (buf[5] == 2 ? " → SPL信号が検出できていません（配線/GBA電源要確認）" : ""));
                        return 1;
                    }
                    Console.Error.WriteLine($"受信エラー: {ec} (グループ{start}, {got}/{total}B)");
                    return 1;
                }
                got += n;
            }
            // 再同期: 先頭に残骸があれば正規ヘッダ位置まで読み捨て、不足分を読み足す
            var p0 = 0;
            while (p0 < LinePacket.PacketSize &&
                   !(buf[p0] == 0xA5 && buf[p0 + 3] == 0 && buf[p0 + 2] < 160))
                p0++;
            if (p0 > 0 && p0 < LinePacket.PacketSize)
            {
                Console.Error.WriteLine($"(再同期: 残骸 {p0}B を破棄)");
                Array.Resize(ref buf, total + p0);
                var got2 = total;
                while (got2 < total + p0)
                {
                    if (reader.Read(buf, got2, total + p0 - got2, 3000, out var n2) != Error.Success || n2 == 0)
                        break;
                    got2 += n2;
                }
            }
            for (var i = 0; i < 32; i++)
            {
                var pkt = LinePacket.Parse(buf.AsSpan(p0 + i * LinePacket.PacketSize, LinePacket.PacketSize));
                if (pkt is not null) lines[pkt.LineNo] = pkt;
                else Console.Error.WriteLine($"\nパケット不正 (グループ{start} #{i})");
            }
            Console.Write($"\r取得中... {Math.Min(start + 32, 160)}/160 ライン");
        }
        Console.WriteLine();

        var px = new (byte R, byte G, byte B)[160, 240];
        var missing = 0;
        for (var y = 0; y < 160; y++)
        {
            if (lines.TryGetValue(y, out var pkt))
                for (var x = 0; x < 240; x++)
                {
                    var cell = Math.Max(0, x - PixelDecoder.SampleOffset);
                    px[y, x] = PixelDecoder.DecodeSample(pkt.Samples[cell]);
                }
            else
                missing++;
        }
        File.WriteAllBytes(outPath, BmpWriter.Encode(px));
        Console.WriteLine($"保存しました: {outPath} (欠落ライン: {missing})");
        return missing == 0 ? 0 : 1;
    }
    case "rawdiag": // 生ピン診断 ('R'): エッジ数/デューティ/GPIOB活動
    {
        using var dev = Open(context);
        if (dev is null) return 1;
        var b = Transact(dev, MakeCmd('R'), 88, timeoutMs: 5000);
        if (b[4] != 'R') { Console.Error.WriteLine("rawdiag 失敗"); return 1; }
        var loops = BitConverter.ToUInt32(b, 40);
        string Pct(int off) => $"{100.0 * BitConverter.ToUInt32(b, off) / loops:F1}%";
        Console.WriteLine($"サンプル数: {loops}");
        Console.WriteLine($"PA0 (DCK): エッジ {BitConverter.ToUInt32(b, 8),8}, H率 {Pct(20)}");
        Console.WriteLine($"PA1 (LP) : エッジ {BitConverter.ToUInt32(b, 12),8}, H率 {Pct(24)}");
        Console.WriteLine($"PA2 (SPS): エッジ {BitConverter.ToUInt32(b, 16),8}, H率 {Pct(28)}");
        Console.WriteLine($"GPIOB    : 変化 {BitConverter.ToUInt32(b, 32),8}, OR=0x{BitConverter.ToUInt16(b, 36):X4}, AND=0x{BitConverter.ToUInt16(b, 38):X4}");
        Console.WriteLine($"プルテスト PA0-2: PD時=0b{b[5]:B3} PU時=0b{b[6]:B3}  (PD時0&PU時1=浮き / 変化なし=外部駆動)");
        Console.WriteLine($"プルテスト GPIOB: PD時=0x{BitConverter.ToUInt16(b, 44):X4} PU時=0x{BitConverter.ToUInt16(b, 46):X4}");
        Console.WriteLine($"PC4(LDR1)/PC5(LDR2): OR=0b{b[80]:B2} AND=0b{b[81]:B2} 変化 {BitConverter.ToUInt32(b, 82)}");
        var names = new Dictionary<int, string> {
            [0]="LDR3",[1]="LDR4",[3]="LDB5",[4]="LDB4",[5]="LDB3",[8]="LDB2",[9]="LDB1",
            [10]="LDR5",[11]="LDG1",[12]="LDG2",[13]="LDG3",[14]="LDG4",[15]="LDG5" };
        Console.Write("GPIOB ビット別エッジ: ");
        foreach (var (bit, name) in names.OrderBy(kv => kv.Key))
        {
            var n = BitConverter.ToUInt16(b, 48 + bit * 2);
            if (n > 0) Console.Write($"{name}(PB{bit})={n} ");
        }
        Console.WriteLine("(表示なし=全ビット変化ゼロ)");
        return 0;
    }
    case "volt": // 'A': DCK/LP/SPS/LDR1/LDR2 の実電圧 (min-max)
    {
        using var dev = Open(context);
        if (dev is null) return 1;
        var b = Transact(dev, MakeCmd('A'), 36, timeoutMs: 5000);
        if (b[4] != 'A') { Console.Error.WriteLine("volt 失敗"); return 1; }
        string[] names = ["PA0 DCK ", "PA1 LP  ", "PA2 SPL ", "PC4 LDR1", "PC5 LDR2", "PA4 SO1 ", "PA5 SO2 "];
        for (var c = 0; c < 7; c++)
        {
            var lo = BitConverter.ToUInt16(b, 8 + c * 4) * 3.3 / 4095;
            var hi = BitConverter.ToUInt16(b, 10 + c * 4) * 3.3 / 4095;
            Console.WriteLine($"{names[c]}: {lo:F2} 〜 {hi:F2} V");
        }
        Console.WriteLine("(音声SO1/SO2: 音が出ていれば min-max が大きく開く(PWM)。0V張り付きなら未接続疑い)");
        return 0;
    }
    case "streamtest": // 'S'を送り2秒間EP4/1/3を読んで統計を出す
    {
        using var dev = Open(context);
        if (dev is null) return 1;
        Write(dev, MakeCmd('S'));
        long[] bytes = new long[3];
        var lineSeen = new HashSet<int>();
        var epids = new[] { ReadEndpointID.Ep04, ReadEndpointID.Ep01, ReadEndpointID.Ep03 };
        var threads = new List<Thread>();
        var stopAt = Environment.TickCount64 + 2000;
        for (var k = 0; k < 3; k++)
        {
            var kk = k;
            var t = new Thread(() =>
            {
                try
                {
                    var rd = dev.OpenEndpointReader(epids[kk]);
                    var b = new byte[65536];
                    while (Environment.TickCount64 < stopAt)
                    {
                        if (rd.Read(b, 300, out var n) != Error.Success) continue;
                        bytes[kk] += n;
                        for (var i = 0; i + 4 <= n; i += LinePacket.PacketSize)
                            if (b[i] == 0xA5)
                                lock (lineSeen) lineSeen.Add(b[i + 2]);
                    }
                }
                catch (Exception ex) { Console.Error.WriteLine($"EP{epids[kk]}: {ex.Message}"); }
            });
            t.Start();
            threads.Add(t);
        }
        foreach (var t in threads) t.Join();
        Write(dev, MakeCmd('X'));
        Console.WriteLine($"EP4: {bytes[0]}B, EP1: {bytes[1]}B, EP3: {bytes[2]}B (2秒)");
        Console.WriteLine($"合計 {(bytes[0] + bytes[1] + bytes[2]) / 2.0 / 1e6:F2} MB/s");
        Console.WriteLine($"観測ライン番号: {lineSeen.Count}種 min={(lineSeen.Count > 0 ? lineSeen.Min() : -1)} max={(lineSeen.Count > 0 ? lineSeen.Max() : -1)}");
        return 0;
    }
    case "framestat": // ストリーミング中のフレームサンプル数統計 ('Q')
    {
        using var dev = Open(context);
        if (dev is null) return 1;
        Write(dev, MakeCmd('S'));
        var epids = new[] { ReadEndpointID.Ep04, ReadEndpointID.Ep01, ReadEndpointID.Ep03 };
        var stop = false;
        var ths = epids.Select(id => new Thread(() =>
        {
            try { var rd = dev.OpenEndpointReader(id); var b = new byte[65536];
                  while (!stop) rd.Read(b, 200, out _); } catch { }
        })).ToList();
        ths.ForEach(t => t.Start());
        Thread.Sleep(4000); // 数百フレーム分の統計を溜める
        // 'Q' の応答(16B)は EP4 のストリーム(512B パケット)に混ざって届く。
        // 読み取りスレッドを止めてから EP4 を自分で読み、"GBACq" を探す
        stop = true;
        ths.ForEach(t => t.Join(500));
        Write(dev, MakeCmd('Q'));
        byte[]? q = null;
        {
            var rd4 = dev.OpenEndpointReader(ReadEndpointID.Ep04);
            var acc = new List<byte>();
            var deadline = Environment.TickCount64 + 3000;
            while (Environment.TickCount64 < deadline && q is null)
            {
                var b = new byte[65536];
                if (rd4.Read(b, 300, out var n) != Error.Success || n == 0) continue;
                acc.AddRange(b.Take(n));
                for (var i = 0; i + 16 <= acc.Count; i++)
                    if (acc[i] == 'G' && acc[i + 1] == 'B' && acc[i + 2] == 'A' && acc[i + 3] == 'C' && acc[i + 4] == 'q')
                    {
                        q = acc.Skip(i).Take(16).ToArray();
                        break;
                    }
                if (acc.Count > 1 << 20) acc.RemoveRange(0, acc.Count - 4096);
            }
        }
        Write(dev, MakeCmd('X'));
        if (q is null)
        {
            Console.Error.WriteLine("'Q' の応答が見つからない");
            return 1;
        }
        var last = BitConverter.ToInt32(q, 8);
        var min = BitConverter.ToInt32(q, 12);
        var max = (int)BitConverter.ToInt16(q, 5);
        Console.WriteLine($"フレームサンプル数-38560: last={last} min={min} max={max}");
        Console.WriteLine("(全て0が理想。非0はVBLANK境界に余分/不足パルスがある証拠)");
        return 0;
    }
    case "cmd":
    {
        using var dev = Open(context);
        if (dev is null) return 1;
        var buf = Transact(dev, MakeCmd(args[1][0]), 8);
        Console.WriteLine($"reply: {Convert.ToHexString(buf.AsSpan(0, 8))}");
        return 0;
    }
    default:
        Console.Error.WriteLine($"unknown command: {args[0]}");
        return 2;
}

static byte[] MakeCmd(char cmd, uint? sizeLE = null)
{
    var pkt = new byte[sizeLE is null ? 5 : 12];
    "GBAC"u8.CopyTo(pkt);
    pkt[4] = (byte)cmd;
    if (sizeLE is not null) BitConverter.GetBytes(sizeLE.Value).CopyTo(pkt, 8);
    return pkt;
}

static IUsbDevice? Open(UsbContext ctx)
{
    var dev = ctx.Find(d => d.VendorId == Vid && d.ProductId == Pid);
    if (dev is null)
    {
        Console.Error.WriteLine($"デバイス {Vid:X4}:{Pid:X4} (GBAcapture) が見つかりません。");
        return null;
    }
    dev.Open();
    dev.ClaimInterface(0);
    // ストリーミング停止 + 前セッションの残骸排水（開いた直後に必ず行う）
    try
    {
        var w = dev.OpenEndpointWriter(WriteEndpointID.Ep05);
        byte[] stop = [(byte)'G', (byte)'B', (byte)'A', (byte)'C', (byte)'X'];
        w.Write(stop, 500, out _);
        var rd = dev.OpenEndpointReader(ReadEndpointID.Ep04);
        var drain = new byte[16384];
        while (rd.Read(drain, 100, out var dn) == Error.Success && dn > 0) { }
    }
    catch { }
    return dev;
}

static IUsbDevice? Reopen(UsbContext ctx, bool expectIapMode, int timeoutMs)
{
    var deadline = Environment.TickCount64 + timeoutMs;
    Thread.Sleep(1500);
    while (Environment.TickCount64 < deadline)
    {
        IUsbDevice? dev = null;
        try
        {
            dev = ctx.Find(d => d.VendorId == Vid && d.ProductId == Pid);
            if (dev is not null)
            {
                dev.Open();
                dev.ClaimInterface(0);
                var (mode, _) = Ping(dev);
                if ((mode == 0xAA) == expectIapMode) return dev;
            }
        }
        catch { /* 列挙途中は失敗し得る。リトライ */ }
        dev?.Dispose(); // 解放しないと以降の open が失敗し続ける
        Thread.Sleep(500);
    }
    return null;
}

static (byte Mode, byte Version) Ping(IUsbDevice dev)
{
    var buf = Transact(dev, MakeCmd('P'), 8);
    if (buf[0] != 'G' || buf[4] != '!') throw new InvalidOperationException("ping 応答不正");
    return (buf[5], buf[6]);
}

static void Write(IUsbDevice dev, byte[] packet)
{
    var writer = dev.OpenEndpointWriter(WriteEndpointID.Ep05);
    var ec = writer.Write(packet, 2000, out var sent);
    if (ec != Error.Success || sent != packet.Length)
        throw new InvalidOperationException($"USB write failed: {ec} ({sent}/{packet.Length})");
}

static byte[] Transact(IUsbDevice dev, byte[] packet, int replyLen, int timeoutMs = 2000)
{
    Write(dev, packet);
    var reader = dev.OpenEndpointReader(ReadEndpointID.Ep04);
    var buf = new byte[512];
    var ec = reader.Read(buf, timeoutMs, out var got);
    if (ec != Error.Success || got < Math.Min(replyLen, 8))
        throw new InvalidOperationException($"USB read failed: {ec} ({got}B)");
    return buf;
}
