#include "RemoteAccess.hpp"

#include <QNetworkAccessManager>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUdpSocket>
#include <QXmlStreamReader>

#include <util/base.h>

#include <algorithm>
#include <memory>

namespace meikou {

namespace {

constexpr int kLeaseSeconds = 7200;      // 2 小時；關掉 Studio 後路由器會自己收回
constexpr int kRenewMs = 30 * 60 * 1000; // 每 30 分鐘續約
const char *kDescription = "Meikou Live Studio";

const QHostAddress kSsdpGroup(QStringLiteral("239.255.255.250"));

QByteArray SearchRequest(const char *st)
{
	return QByteArray("M-SEARCH * HTTP/1.1\r\n"
			  "HOST: 239.255.255.250:1900\r\n"
			  "MAN: \"ssdp:discover\"\r\n"
			  "MX: 2\r\n"
			  "ST: ") +
	       st + "\r\n\r\n";
}

QString XmlValue(const QByteArray &xml, const QString &tag)
{
	QXmlStreamReader r(xml);
	while (!r.atEnd()) {
		r.readNext();
		if (r.isStartElement() && r.name() == tag)
			return r.readElementText().trimmed();
	}
	return QString();
}

} // namespace

RemoteAccess::RemoteAccess(QObject *parent) : QObject(parent)
{
	nam_ = new QNetworkAccessManager(this);
	nam_->setProxy(QNetworkProxy::NoProxy); // 路由器在區網，不走代理
	searchTimer_ = new QTimer(this);
	searchTimer_->setSingleShot(true);
	connect(searchTimer_, &QTimer::timeout, this, &RemoteAccess::OnSearchTimeout);
	renewTimer_ = new QTimer(this);
	renewTimer_->setInterval(kRenewMs);
	connect(renewTimer_, &QTimer::timeout, this, [this] {
		if (state_ == State::Ready)
			AddMappings();
	});
}

RemoteAccess::~RemoteAccess()
{
	// 盡量把開出去的埠收回來（關閉程式時來不及等回應也沒關係，租約 2 小時後會自動失效）
	if (enabled_)
		DeleteMappings();
}

bool RemoteAccess::IsPrivate(const QHostAddress &a)
{
	const quint32 ip = a.toIPv4Address();
	return (ip >> 24) == 10 || (ip >> 20) == ((172u << 4) | 1u) || (ip >> 16) == ((192u << 8) | 168u);
}

bool RemoteAccess::IsSharedRange(const QHostAddress &a)
{
	const quint32 ip = a.toIPv4Address();
	return (ip >> 22) == ((100u << 2) | 1u); // 100.64.0.0/10
}

QStringList RemoteAccess::TailscaleAddresses()
{
	QStringList out;
	for (const QNetworkInterface &nif : QNetworkInterface::allInterfaces()) {
		if (!(nif.flags() & QNetworkInterface::IsUp))
			continue;
		for (const QNetworkAddressEntry &e : nif.addressEntries()) {
			const QHostAddress a = e.ip();
			if (a.protocol() == QAbstractSocket::IPv4Protocol && IsSharedRange(a))
				out << a.toString();
		}
	}
	out.removeDuplicates();
	return out;
}

void RemoteAccess::SetEnabled(bool on, const std::vector<Port> &ports)
{
	ports_ = ports;
	if (on == enabled_) {
		if (on)
			UpdatePorts(ports);
		return;
	}
	enabled_ = on;
	generation_++;
	if (on) {
		Search();
	} else {
		renewTimer_->stop();
		searchTimer_->stop();
		DeleteMappings();
		externalIp_.clear();
		SetState(State::Off);
	}
}

void RemoteAccess::UpdatePorts(const std::vector<Port> &ports)
{
	ports_ = ports;
	if (enabled_ && state_ == State::Ready)
		AddMappings();
	else
		emit Changed();
}

QString RemoteAccess::PortList() const
{
	QStringList udp, tcp;
	for (const Port &p : ports_)
		(p.tcp ? tcp : udp) << QString::number(p.port);
	QStringList parts;
	if (!udp.isEmpty())
		parts << QStringLiteral("UDP %1").arg(udp.join(u'、'));
	if (!tcp.isEmpty())
		parts << QStringLiteral("TCP %1").arg(tcp.join(u'、'));
	return parts.join(QStringLiteral("，"));
}

QString RemoteAccess::StatusText() const
{
	const QString target = localIp_.isEmpty() ? QStringLiteral("這台電腦") : localIp_;
	switch (state_) {
	case State::Off:
		return QStringLiteral("未開啟。手機在同一個 Wi‑Fi 時不需要開。");
	case State::Searching:
		return QStringLiteral("正在請路由器開啟連接埠…");
	case State::Ready:
		return QStringLiteral("已開啟。手機在外面可以掃 QR Code 連回來（外網位址 %1）。").arg(externalIp_);
	case State::NoRouter:
		return QStringLiteral("找不到支援自動開埠（UPnP）的路由器。請在路由器設定把 %1 轉送到 %2，"
				      "或改用 Tailscale。")
			.arg(PortList(), target);
	case State::Failed:
		return QStringLiteral("路由器拒絕自動開埠（%1）。請在路由器設定把 %2 轉送到 %3，或改用 Tailscale。")
			.arg(detail_, PortList(), target);
	case State::SharedIp:
		return QStringLiteral("這個網路沒有自己的對外 IP（%1，可能是電信商共用 IP 或雙層路由器），"
				      "手機從外面連不進來。請改用 Tailscale。")
			.arg(detail_);
	}
	return QString();
}

void RemoteAccess::SetState(State s, const QString &detail)
{
	state_ = s;
	detail_ = detail;
	blog(LOG_INFO, "[remote] %s", StatusText().toUtf8().constData());
	emit Changed();
}

// ---------------------------------------------------------------- 搜尋路由器

void RemoteAccess::Search()
{
	SetState(State::Searching);
	controlUrl_.clear();
	serviceType_.clear();
	fetchingDescription_ = false;
	if (ssdp_)
		ssdp_->deleteLater();
	ssdp_ = new QUdpSocket(this);
	ssdp_->bind(QHostAddress::AnyIPv4, 0);
	connect(ssdp_, &QUdpSocket::readyRead, this, &RemoteAccess::OnSsdp);
	for (const char *st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
			       "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
			       "urn:schemas-upnp-org:service:WANIPConnection:1",
			       "urn:schemas-upnp-org:service:WANPPPConnection:1"})
		ssdp_->writeDatagram(SearchRequest(st), kSsdpGroup, 1900);
	searchTimer_->start(searchTimeoutMs);
}

void RemoteAccess::OnSsdp()
{
	static const QRegularExpression location(QStringLiteral("^LOCATION:\\s*(\\S+)"),
						 QRegularExpression::CaseInsensitiveOption |
							 QRegularExpression::MultilineOption);
	while (ssdp_ && ssdp_->hasPendingDatagrams()) {
		const QNetworkDatagram dg = ssdp_->receiveDatagram(8192);
		const QRegularExpressionMatch m = location.match(QString::fromLatin1(dg.data()));
		if (!m.hasMatch() || fetchingDescription_ || !controlUrl_.isEmpty())
			continue;
		const QUrl url(m.captured(1).trimmed());
		if (url.isValid() && url.scheme() == QLatin1String("http"))
			FetchDescription(url);
	}
}

void RemoteAccess::OnSearchTimeout()
{
	if (state_ == State::Searching && controlUrl_.isEmpty())
		SetState(State::NoRouter);
}

void RemoteAccess::FetchDescription(const QUrl &location)
{
	fetchingDescription_ = true;
	const int gen = generation_;
	QNetworkRequest req(location);
	req.setTransferTimeout(4000);
	QNetworkReply *reply = nam_->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply, location, gen] {
		reply->deleteLater();
		fetchingDescription_ = false;
		if (gen != generation_ || !enabled_)
			return;
		if (reply->error() != QNetworkReply::NoError || !ParseDescription(reply->readAll(), location))
			return; // 等其他路由器回覆或逾時
		searchTimer_->stop();

		// 這台電腦面向路由器的區網 IP（同網段的那張網卡）
		const QHostAddress router(location.host());
		localIp_.clear();
		for (const QNetworkInterface &nif : QNetworkInterface::allInterfaces()) {
			for (const QNetworkAddressEntry &e : nif.addressEntries()) {
				if (e.ip().protocol() != QAbstractSocket::IPv4Protocol || e.prefixLength() <= 0)
					continue;
				if (router.isInSubnet(e.ip(), e.prefixLength())) {
					localIp_ = e.ip().toString();
					break;
				}
			}
			if (!localIp_.isEmpty())
				break;
		}
		if (localIp_.isEmpty()) {
			SetState(State::Failed, QStringLiteral("找不到和路由器同網段的網卡"));
			return;
		}
		QueryExternalIp();
	});
}

bool RemoteAccess::ParseDescription(const QByteArray &xml, const QUrl &location)
{
	QXmlStreamReader r(xml);
	QString urlBase, type, control;
	QString foundType, foundControl;
	while (!r.atEnd()) {
		r.readNext();
		if (!r.isStartElement())
			continue;
		if (r.name() == QLatin1String("URLBase")) {
			urlBase = r.readElementText().trimmed();
		} else if (r.name() == QLatin1String("serviceType")) {
			type = r.readElementText().trimmed();
		} else if (r.name() == QLatin1String("controlURL")) {
			control = r.readElementText().trimmed();
			if (foundControl.isEmpty() &&
			    (type.contains(QLatin1String(":WANIPConnection:")) ||
			     type.contains(QLatin1String(":WANPPPConnection:")))) {
				foundType = type;
				foundControl = control;
			}
		}
	}
	if (foundControl.isEmpty())
		return false;
	const QUrl base = urlBase.isEmpty() ? location : QUrl(urlBase);
	controlUrl_ = base.resolved(QUrl(foundControl));
	serviceType_ = foundType;
	return true;
}

// ---------------------------------------------------------------- SOAP

void RemoteAccess::Soap(const QString &action, const QString &args,
			std::function<void(bool, const QByteArray &)> done)
{
	const QString body = QStringLiteral(
				     "<?xml version=\"1.0\"?>"
				     "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
				     "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
				     "<s:Body><u:%1 xmlns:u=\"%2\">%3</u:%1></s:Body></s:Envelope>")
				     .arg(action, serviceType_, args);
	QNetworkRequest req(controlUrl_);
	req.setTransferTimeout(5000);
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=\"utf-8\""));
	req.setRawHeader("SOAPAction", QStringLiteral("\"%1#%2\"").arg(serviceType_, action).toUtf8());
	QNetworkReply *reply = nam_->post(req, body.toUtf8());
	connect(reply, &QNetworkReply::finished, this, [reply, done] {
		reply->deleteLater();
		const QByteArray data = reply->readAll();
		done(reply->error() == QNetworkReply::NoError, data);
	});
}

void RemoteAccess::QueryExternalIp()
{
	const int gen = generation_;
	Soap(QStringLiteral("GetExternalIPAddress"), QString(), [this, gen](bool ok, const QByteArray &data) {
		if (gen != generation_ || !enabled_)
			return;
		const QString ip = XmlValue(data, QStringLiteral("NewExternalIPAddress"));
		const QHostAddress addr(ip);
		if (!ok || addr.isNull()) {
			SetState(State::Failed, QStringLiteral("查不到對外 IP"));
			return;
		}
		externalIp_ = ip;
		if (IsPrivate(addr) || IsSharedRange(addr)) {
			SetState(State::SharedIp, ip);
			return;
		}
		AddMappings();
	});
}

void RemoteAccess::AddMappings(bool permanentLease)
{
	if (controlUrl_.isEmpty() || localIp_.isEmpty())
		return;
	const int gen = generation_;
	auto pending = std::make_shared<int>((int)ports_.size());
	auto failed = std::make_shared<QString>();
	auto retried = std::make_shared<bool>(false);
	if (*pending == 0) {
		SetState(State::Ready);
		return;
	}
	for (const Port &p : ports_) {
		const QString args =
			QStringLiteral("<NewRemoteHost></NewRemoteHost><NewExternalPort>%1</NewExternalPort>"
				       "<NewProtocol>%2</NewProtocol><NewInternalPort>%1</NewInternalPort>"
				       "<NewInternalClient>%3</NewInternalClient><NewEnabled>1</NewEnabled>"
				       "<NewPortMappingDescription>%4</NewPortMappingDescription>"
				       "<NewLeaseDuration>%5</NewLeaseDuration>")
				.arg(p.port)
				.arg(p.tcp ? QStringLiteral("TCP") : QStringLiteral("UDP"))
				.arg(localIp_, QString::fromLatin1(kDescription))
				.arg(permanentLease ? 0 : kLeaseSeconds);
		Soap(QStringLiteral("AddPortMapping"), args,
		     [this, gen, pending, failed, retried, p, permanentLease](bool ok, const QByteArray &data) {
			     if (gen != generation_ || !enabled_)
				     return;
			     if (!ok) {
				     const QString code = XmlValue(data, QStringLiteral("errorCode"));
				     const QString desc = XmlValue(data, QStringLiteral("errorDescription"));
				     if (code == QLatin1String("725") && !permanentLease) {
					     // 只支援永久租約的舊路由器：整批改用 0（永久）重試一次
					     if (!*retried) {
						     *retried = true;
						     AddMappings(true);
					     }
					     return;
				     }
				     *failed = QStringLiteral("%1 %2 %3").arg(p.tcp ? "TCP" : "UDP").arg(p.port).arg(
					     code.isEmpty() ? QStringLiteral("沒有回應") : code + QStringLiteral(" ") + desc);
			     } else {
				     auto it = std::find_if(mapped_.begin(), mapped_.end(), [&](const Port &m) {
					     return m.port == p.port && m.tcp == p.tcp;
				     });
				     if (it == mapped_.end())
					     mapped_.push_back(p);
			     }
			     if (*retried || --(*pending) != 0)
				     return;
			     if (failed->isEmpty()) {
				     renewTimer_->start();
				     SetState(State::Ready);
			     } else {
				     SetState(State::Failed, *failed);
			     }
		     });
	}
}

void RemoteAccess::DeleteMappings()
{
	if (controlUrl_.isEmpty())
		return;
	for (const Port &p : mapped_) {
		const QString args = QStringLiteral("<NewRemoteHost></NewRemoteHost><NewExternalPort>%1</NewExternalPort>"
						    "<NewProtocol>%2</NewProtocol>")
					     .arg(p.port)
					     .arg(p.tcp ? QStringLiteral("TCP") : QStringLiteral("UDP"));
		Soap(QStringLiteral("DeletePortMapping"), args, [](bool, const QByteArray &) {});
	}
	mapped_.clear();
}

} // namespace meikou
