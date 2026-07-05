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
    case "cmd":
    {
        using var dev = Open(context);
        if (dev is null) return 1;
        var buf = Transact(dev, MakeCmd((char)args[1][0]), 512);
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
    return dev;
}

static IUsbDevice? Reopen(UsbContext ctx, bool expectIapMode, int timeoutMs)
{
    var deadline = Environment.TickCount64 + timeoutMs;
    Thread.Sleep(1500);
    while (Environment.TickCount64 < deadline)
    {
        try
        {
            var dev = ctx.Find(d => d.VendorId == Vid && d.ProductId == Pid);
            if (dev is not null)
            {
                dev.Open();
                dev.ClaimInterface(0);
                var (mode, _) = Ping(dev);
                if ((mode == 0xAA) == expectIapMode) return dev;
                dev.Dispose();
            }
        }
        catch { /* 列挙途中は失敗し得る。リトライ */ }
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
