#include "PhoneLink.hpp"
#include "Engine.hpp"
#include "RemoteAccess.hpp"

#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSettings>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>

#include <util/base.h>

#include <algorithm>
#include <functional>

namespace meikou {

namespace {

QString Q(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}

std::string S(const QString &s)
{
	return s.toUtf8().toStdString();
}

// 10.x、172.16–31.x、192.168.x（QHostAddress::isPrivateUse 要 Qt 6.6 以上，自己判斷）
bool IsPrivateIPv4(const QHostAddress &a)
{
	const quint32 ip = a.toIPv4Address();
	return (ip >> 24) == 10 || (ip >> 20) == ((172u << 4) | 1u) || (ip >> 16) == ((192u << 8) | 168u);
}

constexpr qint64 kPingMs = 5000;
constexpr int kPollMs = 150;
constexpr int kMaxLine = 64 * 1024;

} // namespace

// ---------------------------------------------------------------- 訊號源

bool IsPhoneInput(const Input *in)
{
	if (!in || !in->source || in->isScene)
		return false;
	obs_data_t *s = obs_source_get_settings(in->source);
	const bool phone = s && obs_data_get_bool(s, "meikou_phone");
	obs_data_release(s);
	return phone;
}

int PhonePort(const Input *in)
{
	if (!in || !in->source)
		return 0;
	obs_data_t *s = obs_source_get_settings(in->source);
	const int port = s ? (int)obs_data_get_int(s, "meikou_phone_port") : 0;
	obs_data_release(s);
	return port;
}

std::vector<PhoneCam> PhoneCams(const Engine &engine)
{
	std::vector<PhoneCam> cams;
	for (const auto &in : engine.Inputs()) {
		if (!IsPhoneInput(in.get()))
			continue;
		obs_data_t *s = obs_source_get_settings(in->source);
		const char *pass = s ? obs_data_get_string(s, "meikou_phone_pass") : "";
		cams.push_back({in->uid, in->name, PhonePort(in.get()), pass ? pass : ""});
		obs_data_release(s);
	}
	return cams;
}

std::string NewPhonePass()
{
	// 20 個英數字（SRT 密碼要 10–79 個字元）
	static const char chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
	std::string s;
	for (int i = 0; i < 20; i++)
		s += chars[QRandomGenerator::system()->bounded((int)sizeof(chars) - 1)];
	return s;
}

void EnsurePhonePasses(Engine &engine)
{
	for (const auto &in : engine.Inputs()) {
		if (!IsPhoneInput(in.get()))
			continue;
		obs_data_t *s = obs_source_get_settings(in->source);
		const std::string pass = obs_data_get_string(s, "meikou_phone_pass");
		if (pass.empty()) {
			obs_data_t *upd = obs_data_create();
			ApplyPhoneSettings(upd, (int)obs_data_get_int(s, "meikou_phone_port"), NewPhonePass());
			obs_source_update(in->source, upd);
			obs_data_release(upd);
			blog(LOG_INFO, "[phone] added SRT passphrase to %s", in->name.c_str());
		}
		obs_data_release(s);
	}
}

int NextPhonePort(const Engine &engine)
{
	int port = kPhoneFirstPort;
	const auto cams = PhoneCams(engine);
	while (std::any_of(cams.begin(), cams.end(), [port](const PhoneCam &c) { return c.port == port; }))
		port++;
	return port;
}

void ApplyPhoneSettings(obs_data_t *s, int port, const std::string &pass)
{
	// SRT 監聽：手機（caller）連進來就有畫面，斷線後自動重新等待；AES-128 加密
	std::string url = "srt://0.0.0.0:" + std::to_string(port) + "?mode=listener&latency=200000";
	if (!pass.empty())
		url += "&passphrase=" + pass + "&pbkeylen=16";
	obs_data_set_string(s, "meikou_phone_pass", pass.c_str());
	obs_data_set_bool(s, "is_local_file", false);
	obs_data_set_string(s, "input", url.c_str());
	obs_data_set_string(s, "input_format", "mpegts");
	obs_data_set_bool(s, "restart_on_activate", false);
	obs_data_set_bool(s, "close_when_inactive", false);
	obs_data_set_int(s, "reconnect_delay_sec", 1);
	obs_data_set_int(s, "buffering_mb", 1);
	obs_data_set_bool(s, "hw_decode", true);
	obs_data_set_bool(s, "meikou_phone", true);
	obs_data_set_int(s, "meikou_phone_port", port);
}

namespace {

// 場景（畫面組合）裡顯示中的圖層有沒有這個訊號源；場景可能再包場景，最多往下找 4 層
bool Shows(const Engine &engine, Input *in, const std::string &uid, int depth)
{
	if (!in || depth > 4)
		return false;
	if (in->uid == uid)
		return true;
	if (!in->isScene)
		return false;
	for (const auto &layer : engine.SceneLayers(in)) {
		if (!layer.visible || layer.isOverlay)
			continue;
		if (layer.uid == uid)
			return true;
		for (const auto &other : engine.Inputs())
			if (other->uid == layer.uid && other->isScene && Shows(engine, other.get(), uid, depth + 1))
				return true;
	}
	return false;
}

} // namespace

Tally TallyOf(Engine &engine, const std::string &uid)
{
	if (Shows(engine, engine.Program(), uid, 0))
		return Tally::Program;
	if (Shows(engine, engine.Preview(), uid, 0))
		return Tally::Preview;
	return Tally::Off;
}

const char *TallyCode(Tally t)
{
	switch (t) {
	case Tally::Program:
		return "pgm";
	case Tally::Preview:
		return "pvw";
	default:
		return "off";
	}
}

// ---------------------------------------------------------------- 網路

PhoneLink::PhoneLink(Engine &engine, QObject *parent) : QObject(parent), engine_(engine)
{
	clock_.start();
	remote_ = new RemoteAccess(this);
}

void PhoneLink::SetRemoteEnabled(bool on)
{
	QSettings().setValue("phone/remote", on);
	EnsurePhonePasses(engine_);
	std::vector<RemoteAccess::Port> ports{{kControlPort, true}};
	for (const auto &cam : PhoneCams(engine_))
		ports.push_back({cam.port, false});
	remote_->SetEnabled(on, ports);
}

void PhoneLink::RefreshRemotePorts()
{
	if (remote_->Enabled())
		SetRemoteEnabled(true);
}

PhoneLink::~PhoneLink()
{
	Stop();
}

bool PhoneLink::Start(QString &error)
{
	if (running_)
		return true;
	udp_ = new QUdpSocket(this);
	if (!udp_->bind(QHostAddress::AnyIPv4, kDiscoveryPort,
			QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
		error = QStringLiteral("手機搜尋埠 %1 無法使用：%2").arg(kDiscoveryPort).arg(udp_->errorString());
	}
	connect(udp_, &QUdpSocket::readyRead, this, &PhoneLink::OnDatagrams);

	tcp_ = new QTcpServer(this);
	if (!tcp_->listen(QHostAddress::AnyIPv4, kControlPort)) {
		const QString e = QStringLiteral("Tally 埠 %1 無法使用：%2").arg(kControlPort).arg(tcp_->errorString());
		error = error.isEmpty() ? e : error + QStringLiteral("；") + e;
	}
	connect(tcp_, &QTcpServer::newConnection, this, &PhoneLink::OnNewConnection);

	timer_ = new QTimer(this);
	timer_->setInterval(kPollMs);
	connect(timer_, &QTimer::timeout, this, &PhoneLink::Poll);
	timer_->start();

	running_ = tcp_->isListening() || udp_->state() == QAbstractSocket::BoundState;
	EnsurePhonePasses(engine_);
	error_ = error;
	blog(error.isEmpty() ? LOG_INFO : LOG_WARNING, "[phone] link started (udp %d, tcp %d) %s", (int)kDiscoveryPort,
	     (int)kControlPort, error.toUtf8().constData());
	return error.isEmpty();
}

void PhoneLink::Stop()
{
	if (timer_)
		timer_->stop();
	for (auto &c : clients_) {
		if (c.sock) {
			c.sock->disconnect(this);
			c.sock->abort();
			c.sock->deleteLater();
		}
	}
	clients_.clear();
	if (tcp_)
		tcp_->close();
	if (udp_)
		udp_->close();
	running_ = false;
}

QStringList PhoneLink::LocalAddresses()
{
	QStringList preferred, others;
	for (const QNetworkInterface &nif : QNetworkInterface::allInterfaces()) {
		const auto f = nif.flags();
		if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::IsRunning) ||
		    (f & QNetworkInterface::IsLoopBack))
			continue;
		for (const QNetworkAddressEntry &e : nif.addressEntries()) {
			const QHostAddress a = e.ip();
			if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLoopback() || a.isLinkLocal())
				continue;
			const QString s = a.toString();
			if (s.startsWith(QLatin1String("198.18.")))
				continue; // 部分 VPN／代理軟體的虛擬網段
			(IsPrivateIPv4(a) ? preferred : others).append(s);
		}
	}
	preferred.removeDuplicates();
	others.removeDuplicates();
	return preferred + others;
}

QStringList PhoneLink::QrHosts() const
{
	// 手機會同時試連這些位址，哪個先通就用哪個：區網（最多 2 個）、Tailscale、外網
	QStringList hosts;
	for (const QString &h : LocalAddresses()) {
		const QHostAddress a(h);
		if (RemoteAccess::IsPrivate(a) && hosts.size() < 2)
			hosts << h;
	}
	for (const QString &h : RemoteAccess::TailscaleAddresses().mid(0, 1))
		hosts << h;
	if (remote_ && remote_->GetState() == RemoteAccess::State::Ready && !remote_->ExternalIp().isEmpty())
		hosts << remote_->ExternalIp();
	if (hosts.isEmpty())
		hosts = LocalAddresses().mid(0, 2);
	hosts.removeDuplicates();
	return hosts;
}

QString PhoneLink::QrText(const PhoneCam &cam) const
{
	return QStringLiteral("meikoulive://cam?v=1&h=%1&p=%2&c=%3&id=%4&k=%5&n=%6")
		.arg(QrHosts().join(u','))
		.arg(cam.port)
		.arg(kControlPort)
		.arg(QString::fromUtf8(QUrl::toPercentEncoding(Q(cam.uid))))
		.arg(QString::fromUtf8(QUrl::toPercentEncoding(Q(cam.pass))))
		.arg(QString::fromUtf8(QUrl::toPercentEncoding(Q(cam.name))));
}

QString PhoneLink::DeviceFor(const std::string &uid) const
{
	for (const auto &c : clients_)
		if (c.uid == uid)
			return c.device.isEmpty() ? QStringLiteral("手機") : c.device;
	return QString();
}

int PhoneLink::ConnectedCount() const
{
	int n = 0;
	for (const auto &c : clients_)
		if (!c.uid.empty())
			n++;
	return n;
}

void PhoneLink::OnDatagrams()
{
	while (udp_ && udp_->hasPendingDatagrams()) {
		const QNetworkDatagram dg = udp_->receiveDatagram(4096);
		if (!dg.isValid())
			continue;
		const QJsonObject req = QJsonDocument::fromJson(dg.data()).object();
		if (req.value(QStringLiteral("t")).toString() != QLatin1String("discover"))
			continue;
		EnsurePhonePasses(engine_);
		QJsonArray cams;
		for (const auto &cam : PhoneCams(engine_)) {
			QJsonObject o;
			o["id"] = Q(cam.uid);
			o["name"] = Q(cam.name);
			o["port"] = cam.port;
			o["key"] = Q(cam.pass); // 只在區網回覆；外網要掃 QR Code
			o["busy"] = !DeviceFor(cam.uid).isEmpty();
			cams.append(o);
		}
		QJsonObject reply;
		reply["t"] = QStringLiteral("studio");
		reply["v"] = 1;
		reply["name"] = QHostInfo::localHostName();
		reply["app"] = QStringLiteral("Meikou Live Studio");
		reply["version"] = QStringLiteral(MEIKOU_VERSION);
		reply["ctl"] = (int)kControlPort;
		reply["cams"] = cams;
		udp_->writeDatagram(QJsonDocument(reply).toJson(QJsonDocument::Compact), dg.senderAddress(),
				    (quint16)dg.senderPort());
	}
}

void PhoneLink::OnNewConnection()
{
	while (tcp_ && tcp_->hasPendingConnections()) {
		QTcpSocket *sock = tcp_->nextPendingConnection();
		sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
		Client c;
		c.sock = sock;
		c.lastPing = clock_.elapsed();
		clients_.push_back(c);
		connect(sock, &QTcpSocket::readyRead, this, [this, sock] { OnReadyRead(sock); });
		connect(sock, &QTcpSocket::disconnected, this, [this, sock] { OnDisconnected(sock); });
	}
}

PhoneLink::Client *PhoneLink::FindClient(QTcpSocket *sock)
{
	for (auto &c : clients_)
		if (c.sock == sock)
			return &c;
	return nullptr;
}

void PhoneLink::OnReadyRead(QTcpSocket *sock)
{
	Client *c = FindClient(sock);
	if (!c)
		return;
	c->buf.append(sock->readAll());
	if (c->buf.size() > kMaxLine) {
		sock->abort();
		return;
	}
	bool changed = false;
	int nl;
	while ((nl = c->buf.indexOf('\n')) >= 0) {
		const QByteArray line = c->buf.left(nl).trimmed();
		c->buf.remove(0, nl + 1);
		if (line.isEmpty())
			continue;
		const QJsonObject msg = QJsonDocument::fromJson(line).object();
		if (msg.value(QStringLiteral("t")).toString() != QLatin1String("hello"))
			continue;
		const std::string uid = S(msg.value(QStringLiteral("id")).toString());
		const auto cams = PhoneCams(engine_);
		const auto it = std::find_if(cams.begin(), cams.end(), [&](const PhoneCam &cam) { return cam.uid == uid; });
		if (it == cams.end()) {
			QJsonObject err;
			err["t"] = QStringLiteral("error");
			err["msg"] = QStringLiteral("導播機找不到這個手機攝影機訊號源，請重新搜尋或掃描 QR Code");
			Send(sock, err);
			continue;
		}
		if (!it->pass.empty() && S(msg.value(QStringLiteral("key")).toString()) != it->pass) {
			QJsonObject err;
			err["t"] = QStringLiteral("error");
			err["msg"] = QStringLiteral("密碼不符，請重新掃描導播機的 QR Code");
			Send(sock, err);
			continue;
		}
		c->uid = uid;
		c->device = msg.value(QStringLiteral("device")).toString().left(40);
		c->lastTally = -1; // 馬上送一次目前狀態
		changed = true;
		blog(LOG_INFO, "[phone] %s connected as %s", c->device.toUtf8().constData(), it->name.c_str());
	}
	if (changed) {
		Poll();
		emit ClientsChanged();
	}
}

void PhoneLink::OnDisconnected(QTcpSocket *sock)
{
	const auto it = std::find_if(clients_.begin(), clients_.end(), [sock](const Client &c) { return c.sock == sock; });
	if (it == clients_.end())
		return;
	const bool hadUid = !it->uid.empty();
	clients_.erase(it);
	sock->deleteLater();
	if (hadUid)
		emit ClientsChanged();
}

void PhoneLink::Send(QTcpSocket *sock, const QJsonObject &obj)
{
	if (!sock || sock->state() != QAbstractSocket::ConnectedState)
		return;
	QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
	data.append('\n');
	sock->write(data);
}

void PhoneLink::Poll()
{
	const qint64 now = clock_.elapsed();
	std::vector<QTcpSocket *> stale;
	for (auto &c : clients_) {
		if (c.uid.empty()) {
			// 連上後 10 秒都沒打招呼：不是我們的手機，斷開
			if (now - c.lastPing > 10000 && c.sock)
				stale.push_back(c.sock);
			continue;
		}
		const Tally t = TallyOf(engine_, c.uid);
		if ((int)t != c.lastTally) {
			c.lastTally = (int)t;
			QJsonObject o;
			o["t"] = QStringLiteral("tally");
			o["state"] = QString::fromLatin1(TallyCode(t));
			if (Input *in = engine_.FindInput(c.uid))
				o["name"] = Q(in->name);
			Send(c.sock, o);
		}
		if (now - c.lastPing >= kPingMs) {
			c.lastPing = now;
			QJsonObject o;
			o["t"] = QStringLiteral("ping");
			Send(c.sock, o);
		}
	}
	// abort() 會馬上觸發 disconnected 並從 clients_ 移除，所以放到迴圈外面
	for (QTcpSocket *sock : stale)
		sock->abort();
}

} // namespace meikou
