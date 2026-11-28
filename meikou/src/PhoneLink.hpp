#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>

#include <obs.h>

#include <string>
#include <vector>

class QJsonObject;
class QTcpServer;
class QTcpSocket;
class QTimer;
class QUdpSocket;

namespace meikou {

class Engine;
struct Input;

// ---------------------------------------------------------------------------
// 手機攝影機（Meikou Live Mobile）
//
// 手機攝影機就是一個「SRT 監聽中」的網路影片訊號源（ffmpeg_source），
// 設定裡多存 meikou_phone=true 與 meikou_phone_port。手機用 SRT 把畫面送到這個埠。
//
// 區網協定（v1，UTF-8 JSON）：
//   搜尋  UDP 47800：手機廣播 {"t":"discover","v":1}
//                    導播機回覆 {"t":"studio","v":1,"name":電腦名稱,"ctl":47801,
//                                "cams":[{"id","name","port","key","busy"}]}
//   Tally TCP 47801：一行一個 JSON。
//                    手機 → {"t":"hello","v":1,"id":訊號源 uid,"key":加密密碼,"name":攝影機名稱,"device":手機型號}
//                    導播 → {"t":"tally","state":"pgm"|"pvw"|"off","name":訊號源名稱}（狀態改變時）
//                    導播 → {"t":"ping"}（每 5 秒）、{"t":"error","msg":…}
//   QR Code：meikoulive://cam?v=1&h=IP1,IP2&p=SRT埠&c=47801&id=uid&k=密碼&n=名稱
//   每個手機攝影機有自己的 SRT 加密密碼（AES-128），手機在外網連線時也安全。
// ---------------------------------------------------------------------------

enum class Tally { Off = 0, Preview = 1, Program = 2 };

struct PhoneCam {
	std::string uid;
	std::string name;
	int port = 0;
	std::string pass; // SRT 加密密碼
};

constexpr int kPhoneFirstPort = 9001;

bool IsPhoneInput(const Input *in);
int PhonePort(const Input *in);
std::string NewPhonePass();
// 舊版建立、還沒有加密密碼的手機攝影機補上密碼
void EnsurePhonePasses(Engine &engine);
std::vector<PhoneCam> PhoneCams(const Engine &engine);
int NextPhonePort(const Engine &engine);
// 填好手機攝影機（SRT 監聽）的 ffmpeg_source 設定
void ApplyPhoneSettings(obs_data_t *settings, int port, const std::string &pass);
// 這個訊號源目前在播出（PGM）、預覽（PVW）或都沒有；場景裡顯示中的圖層也算
Tally TallyOf(Engine &engine, const std::string &uid);
const char *TallyCode(Tally t);

class RemoteAccess;

class PhoneLink : public QObject {
	Q_OBJECT

public:
	static constexpr quint16 kDiscoveryPort = 47800;
	static constexpr quint16 kControlPort = 47801;

	explicit PhoneLink(Engine &engine, QObject *parent = nullptr);
	~PhoneLink() override;

	bool Start(QString &error);
	void Stop();
	bool Running() const { return running_; }
	QString Error() const { return error_; }

	// 這台電腦在區網的 IPv4 位址（私有網段排前面）
	static QStringList LocalAddresses();
	// QR Code：區網位址、Tailscale 位址，外網連線開啟時再加上對外 IP
	QString QrText(const PhoneCam &cam) const;
	QStringList QrHosts() const;

	// 外網連線（UPnP 自動開埠）；設定會記住
	RemoteAccess *Remote() const { return remote_; }
	void SetRemoteEnabled(bool on);
	void RefreshRemotePorts();
	QString DeviceFor(const std::string &uid) const; // 沒連線時是空字串
	int ConnectedCount() const;

signals:
	void ClientsChanged();

private:
	struct Client {
		QTcpSocket *sock = nullptr;
		QByteArray buf;
		std::string uid;
		QString device;
		int lastTally = -1;
		qint64 lastPing = 0;
	};

	void OnDatagrams();
	void OnNewConnection();
	void OnReadyRead(QTcpSocket *sock);
	void OnDisconnected(QTcpSocket *sock);
	void Poll();
	void Send(QTcpSocket *sock, const QJsonObject &obj);
	Client *FindClient(QTcpSocket *sock);

	Engine &engine_;
	RemoteAccess *remote_ = nullptr;
	QUdpSocket *udp_ = nullptr;
	QTcpServer *tcp_ = nullptr;
	QTimer *timer_ = nullptr;
	std::vector<Client> clients_;
	QString error_;
	bool running_ = false;
	QElapsedTimer clock_;
};

} // namespace meikou
