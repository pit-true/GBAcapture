using LibUsbDotNet;
using LibUsbDotNet.LibUsb;
using LibUsbDotNet.Main;

// GBAcapture デバイス操作 CLI
//   ping             : 疎通確認（FWバージョンを表示）
//   enter-bootloader : ISPブートローダへソフト突入（以後 wchisp で書き込み可能）
const int Vid = 0x1209;
const int Pid = 0xC001;

if (args.Length < 1)
{
    Console.Error.WriteLine("usage: gbacap-cli <ping|enter-bootloader>");
    return 2;
}

using var context = new UsbContext();
using var device = context.Find(d => d.VendorId == Vid && d.ProductId == Pid);
if (device is null)
{
    Console.Error.WriteLine($"デバイス {Vid:X4}:{Pid:X4} (GBAcapture) が見つかりません。");
    Console.Error.WriteLine("接続と WinUSB ドライバ割当 (Zadig) を確認してください。");
    return 1;
}

device.Open();
device.ClaimInterface(0);
var writer = device.OpenEndpointWriter(WriteEndpointID.Ep05);

switch (args[0])
{
    case "ping":
    {
        return Ping(device, writer) ? 0 : 1;
    }
    case "test-bootloader":
    {
        // 同一USBハンドルを保持したまま B 前後で ping し、リセットの有無を判定する。
        // リセットが起きていればハンドルは死ぬ(2回目pingがパイプエラーになる)はず。
        Console.WriteLine($"[1] 事前ping: {(Ping(device, writer) ? "OK" : "NG")}");
        Send(writer, (byte)'B');
        Console.WriteLine("[2] B 送信済み。3秒待機...");
        Thread.Sleep(3000);
        try
        {
            Console.WriteLine($"[3] 事後ping(同一ハンドル): {(Ping(device, writer) ? "OK" : "NG")}");
            Console.WriteLine("→ ハンドル生存 = デバイスはリセットしていない (Bが処理されていない)");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[3] 事後ping例外: {ex.Message}");
            Console.WriteLine("→ ハンドル死亡 = リセットは発生した (ブートローダに弾かれてアプリ復帰)");
        }
        return 0;
    }
    case "enter-bootloader":
    case "enter-bootloader-c": // KEYRアンロック込みの変種
    {
        Send(writer, (byte)(args[0].EndsWith("-c") ? 'C' : 'B'));
        Console.WriteLine("ブートローダ突入コマンド送信済み。数秒後に wchisp が使えます。");
        return 0;
    }
    case "cmd": // 任意コマンド送信+応答ダンプ (診断用): cmd S / cmd T など
    {
        Send(writer, (byte)args[1][0]);
        var reader = device.OpenEndpointReader(ReadEndpointID.Ep04);
        var buf = new byte[512];
        var ec = reader.Read(buf, 2000, out var got);
        Console.WriteLine($"reply: {ec}, {got}B: {Convert.ToHexString(buf.AsSpan(0, Math.Min(got, 16)))}");
        return 0;
    }
    default:
        Console.Error.WriteLine($"unknown command: {args[0]}");
        return 2;
}

static bool Ping(IUsbDevice device, UsbEndpointWriter writer)
{
    Send(writer, (byte)'P');
    var reader = device.OpenEndpointReader(ReadEndpointID.Ep04);
    var buf = new byte[512];
    var ec = reader.Read(buf, 2000, out var got);
    Console.WriteLine($"  raw: {ec}, {got}B: {Convert.ToHexString(buf.AsSpan(0, Math.Min(got, 16)))}");
    if (ec != Error.Success || got < 7 || buf[0] != 'G' || buf[4] != '!')
        return false;
    Console.WriteLine($"  PONG: FW v{buf[5]}.{buf[6]}");
    return true;
}

static void Send(UsbEndpointWriter writer, byte cmd)
{
    byte[] packet = [(byte)'G', (byte)'B', (byte)'A', (byte)'C', cmd];
    var ec = writer.Write(packet, 2000, out var sent);
    if (ec != Error.Success || sent != packet.Length)
        throw new InvalidOperationException($"USB write failed: {ec} ({sent}/{packet.Length})");
}
