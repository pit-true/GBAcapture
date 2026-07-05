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
        Send(writer, (byte)'P');
        var reader = device.OpenEndpointReader(ReadEndpointID.Ep04);
        var buf = new byte[512];
        var ec = reader.Read(buf, 2000, out var got);
        if (ec != Error.Success || got < 7 || buf[0] != 'G' || buf[4] != '!')
        {
            Console.Error.WriteLine($"ping 失敗: {ec}, {got} bytes");
            return 1;
        }
        Console.WriteLine($"PONG: FW v{buf[5]}.{buf[6]}");
        return 0;
    }
    case "enter-bootloader":
    {
        Send(writer, (byte)'B');
        Console.WriteLine("ブートローダ突入コマンド送信済み。数秒後に wchisp が使えます。");
        return 0;
    }
    default:
        Console.Error.WriteLine($"unknown command: {args[0]}");
        return 2;
}

static void Send(UsbEndpointWriter writer, byte cmd)
{
    byte[] packet = [(byte)'G', (byte)'B', (byte)'A', (byte)'C', cmd];
    var ec = writer.Write(packet, 2000, out var sent);
    if (ec != Error.Success || sent != packet.Length)
        throw new InvalidOperationException($"USB write failed: {ec} ({sent}/{packet.Length})");
}
