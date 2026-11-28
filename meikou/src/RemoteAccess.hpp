#pragma once

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <vector>

class QNetworkAccessManager;
class QTimer;
class QUdpSocket;

namespace meikou {

// ---------------------------------------------------------------------------
// 外網連線：讓在外面用 4G／5G 的手機連回導播機
//
// 1. UPnP：找到家裡的路由器（SSDP），請它把 SRT（UDP）與 Tally（TCP 47801）的埠
//    轉到這台電腦，並查出對外 IP。租約 2 小時，每 30 分鐘續約；關閉時刪除。
// 2. 對外 IP 是私有位址（雙層路由器）或電信商共用 IP（100.64.0.0/10）時，外面連不進來，
//    改用 Tailscale：手機與電腦登入同一個帳號，就像在同一個網路。
// ---------------------------------------------------------------------------

class RemoteAccess : public QObject {
	Q_OBJECT

public:
	enum class State { Off, Searching, Ready, NoRouter, Failed, SharedIp };

	struct Port {
		int port = 0;
		bool tcp = false;
	};

	explicit RemoteAccess(QObject *parent = nullptr);
	~RemoteAccess() override;

	void SetEnabled(bool on, const std::vector<Port> &ports);
	void UpdatePorts(const std::vector<Port> &ports);
	bool Enabled() const { return enabled_; }
	State GetState() const { return state_; }
	QString ExternalIp() const { return externalIp_; }
	QString LocalIp() const { return localIp_; }
	QString StatusText() const;

	// Tailscale（100.64.0.0/10）位址
	static QStringList TailscaleAddresses();
	// 10.x、172.16–31.x、192.168.x
	static bool IsPrivate(const QHostAddress &a);
	// 100.64.0.0/10：電信商共用 IP，也是 Tailscale 用的網段
	static bool IsSharedRange(const QHostAddress &a);

	// 搜尋路由器的等待時間（自我巡檢會縮短）
	int searchTimeoutMs = 3500;

signals:
	void Changed();

private:
	void Search();
	void OnSsdp();
	void OnSearchTimeout();
	void FetchDescription(const QUrl &location);
	bool ParseDescription(const QByteArray &xml, const QUrl &location);
	void Soap(const QString &action, const QString &args, std::function<void(bool, const QByteArray &)> done);
	void AddMappings(bool permanentLease = false);
	void DeleteMappings();
	void QueryExternalIp();
	void SetState(State s, const QString &detail = QString());
	QString PortList() const;

	QNetworkAccessManager *nam_ = nullptr;
	QUdpSocket *ssdp_ = nullptr;
	QTimer *searchTimer_ = nullptr;
	QTimer *renewTimer_ = nullptr;
	bool enabled_ = false;
	State state_ = State::Off;
	QString detail_;
	std::vector<Port> ports_;
	std::vector<Port> mapped_;
	QUrl controlUrl_;
	QString serviceType_;
	QString localIp_;
	QString externalIp_;
	bool fetchingDescription_ = false;
	int generation_ = 0;
};

} // namespace meikou
