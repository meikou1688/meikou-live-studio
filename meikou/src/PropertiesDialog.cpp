#include "PropertiesDialog.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace meikou {

namespace {

QString Q(const char *s)
{
	return QString::fromUtf8(s ? s : "");
}

QColor FromAbgr(long long v)
{
	uint32_t c = (uint32_t)v;
	return QColor(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF);
}

long long ToAbgr(const QColor &c)
{
	return ((uint32_t)c.alpha() << 24) | ((uint32_t)c.blue() << 16) | ((uint32_t)c.green() << 8) | (uint32_t)c.red();
}

} // namespace

PropertiesDialog::PropertiesDialog(obs_source_t *source, const QString &title, QWidget *parent)
	: QDialog(parent),
	  source_(obs_source_get_ref(source))
{
	setWindowTitle(title);
	resize(560, 640);
	settings_ = obs_source_get_settings(source_);
	original_ = Q(obs_data_get_json(settings_));

	auto *v = new QVBoxLayout(this);
	scroll_ = new QScrollArea;
	scroll_->setWidgetResizable(true);
	v->addWidget(scroll_, 1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
					     QDialogButtonBox::RestoreDefaults);
	buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("確定"));
	buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
	buttons->button(QDialogButtonBox::RestoreDefaults)->setText(QStringLiteral("恢復預設值"));
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] {
		obs_data_clear(settings_);
		obs_source_update(source_, settings_);
		Rebuild();
	});
	v->addWidget(buttons);

	Rebuild();
}

PropertiesDialog::~PropertiesDialog()
{
	if (props_)
		obs_properties_destroy(props_);
	obs_data_release(settings_);
	obs_source_release(source_);
}

void PropertiesDialog::reject()
{
	obs_data_t *orig = obs_data_create_from_json(original_.toUtf8().constData());
	if (orig) {
		obs_source_reset_settings(source_, orig);
		obs_data_release(orig);
	}
	QDialog::reject();
}

void PropertiesDialog::Rebuild()
{
	rebuildPending_ = false;
	if (props_)
		obs_properties_destroy(props_);
	props_ = obs_source_properties(source_);

	auto *body = new QWidget;
	auto *form = new QFormLayout(body);
	form->setLabelAlignment(Qt::AlignRight);
	if (props_)
		AddProperties(props_, form);
	else
		form->addRow(new QLabel(QStringLiteral("這個來源沒有可以調整的設定。")));
	scroll_->setWidget(body);
}

void PropertiesDialog::Changed(obs_property_t *p)
{
	obs_source_update(source_, settings_);
	if (obs_property_modified(p, settings_) && !rebuildPending_) {
		rebuildPending_ = true;
		QTimer::singleShot(0, this, [this] { Rebuild(); });
	}
}

void PropertiesDialog::AddProperties(obs_properties_t *props, QFormLayout *form)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		if (!obs_property_visible(p))
			continue;
		const char *name = obs_property_name(p);
		const QString label = Q(obs_property_description(p));
		QWidget *w = nullptr;

		switch (obs_property_get_type(p)) {
		case OBS_PROPERTY_BOOL: {
			auto *cb = new QCheckBox(label);
			cb->setChecked(obs_data_get_bool(settings_, name));
			connect(cb, &QCheckBox::toggled, this, [this, p, name](bool on) {
				obs_data_set_bool(settings_, name, on);
				Changed(p);
			});
			form->addRow(QString(), cb);
			w = cb;
			break;
		}
		case OBS_PROPERTY_INT: {
			auto *sb = new QSpinBox;
			sb->setRange(obs_property_int_min(p), obs_property_int_max(p));
			sb->setSingleStep(std::max(1, obs_property_int_step(p)));
			sb->setSuffix(Q(obs_property_int_suffix(p)));
			sb->setValue((int)obs_data_get_int(settings_, name));
			connect(sb, &QSpinBox::valueChanged, this, [this, p, name](int v) {
				obs_data_set_int(settings_, name, v);
				Changed(p);
			});
			form->addRow(label, sb);
			w = sb;
			break;
		}
		case OBS_PROPERTY_FLOAT: {
			auto *sb = new QDoubleSpinBox;
			sb->setRange(obs_property_float_min(p), obs_property_float_max(p));
			sb->setSingleStep(obs_property_float_step(p) > 0 ? obs_property_float_step(p) : 0.1);
			sb->setDecimals(2);
			sb->setSuffix(Q(obs_property_float_suffix(p)));
			sb->setValue(obs_data_get_double(settings_, name));
			connect(sb, &QDoubleSpinBox::valueChanged, this, [this, p, name](double v) {
				obs_data_set_double(settings_, name, v);
				Changed(p);
			});
			form->addRow(label, sb);
			w = sb;
			break;
		}
		case OBS_PROPERTY_TEXT: {
			enum obs_text_type tt = obs_property_text_type(p);
			if (tt == OBS_TEXT_INFO) {
				auto *lb = new QLabel(Q(obs_data_get_string(settings_, name)).isEmpty()
							      ? label
							      : Q(obs_data_get_string(settings_, name)));
				lb->setWordWrap(true);
				form->addRow(lb);
				w = lb;
			} else if (tt == OBS_TEXT_MULTILINE) {
				auto *te = new QPlainTextEdit(Q(obs_data_get_string(settings_, name)));
				te->setMinimumHeight(90);
				connect(te, &QPlainTextEdit::textChanged, this, [this, p, name, te] {
					obs_data_set_string(settings_, name, te->toPlainText().toUtf8().constData());
					Changed(p);
				});
				form->addRow(label, te);
				w = te;
			} else {
				auto *le = new QLineEdit(Q(obs_data_get_string(settings_, name)));
				if (tt == OBS_TEXT_PASSWORD)
					le->setEchoMode(QLineEdit::Password);
				connect(le, &QLineEdit::editingFinished, this, [this, p, name, le] {
					obs_data_set_string(settings_, name, le->text().toUtf8().constData());
					Changed(p);
				});
				form->addRow(label, le);
				w = le;
			}
			break;
		}
		case OBS_PROPERTY_PATH: {
			auto *row = new QWidget;
			auto *h = new QHBoxLayout(row);
			h->setContentsMargins(0, 0, 0, 0);
			auto *le = new QLineEdit(Q(obs_data_get_string(settings_, name)));
			auto *browse = new QPushButton(QStringLiteral("瀏覽…"));
			h->addWidget(le, 1);
			h->addWidget(browse);
			connect(le, &QLineEdit::editingFinished, this, [this, p, name, le] {
				obs_data_set_string(settings_, name, le->text().toUtf8().constData());
				Changed(p);
			});
			connect(browse, &QPushButton::clicked, this, [this, p, name, le] {
				QString path;
				const QString start = le->text().isEmpty() ? Q(obs_property_path_default_path(p)) : le->text();
				switch (obs_property_path_type(p)) {
				case OBS_PATH_DIRECTORY:
					path = QFileDialog::getExistingDirectory(this, QString(), start);
					break;
				case OBS_PATH_FILE_SAVE:
					path = QFileDialog::getSaveFileName(this, QString(), start,
									    Q(obs_property_path_filter(p)));
					break;
				default:
					path = QFileDialog::getOpenFileName(this, QString(), start,
									    Q(obs_property_path_filter(p)));
					break;
				}
				if (path.isEmpty())
					return;
				le->setText(path);
				obs_data_set_string(settings_, name, path.toUtf8().constData());
				Changed(p);
			});
			form->addRow(label, row);
			w = row;
			break;
		}
		case OBS_PROPERTY_LIST: {
			auto *cb = new QComboBox;
			const enum obs_combo_format fmt = obs_property_list_format(p);
			const bool editable = obs_property_list_type(p) == OBS_COMBO_TYPE_EDITABLE;
			cb->setEditable(editable);
			const size_t count = obs_property_list_item_count(p);
			int current = -1;
			for (size_t i = 0; i < count; i++) {
				const QString itemName = Q(obs_property_list_item_name(p, i));
				QVariant data;
				bool match = false;
				switch (fmt) {
				case OBS_COMBO_FORMAT_INT: {
					long long v = obs_property_list_item_int(p, i);
					data = QVariant::fromValue(v);
					match = v == obs_data_get_int(settings_, name);
					break;
				}
				case OBS_COMBO_FORMAT_FLOAT: {
					double v = obs_property_list_item_float(p, i);
					data = v;
					match = v == obs_data_get_double(settings_, name);
					break;
				}
				case OBS_COMBO_FORMAT_BOOL: {
					bool v = obs_property_list_item_bool(p, i);
					data = v;
					match = v == obs_data_get_bool(settings_, name);
					break;
				}
				default: {
					QString v = Q(obs_property_list_item_string(p, i));
					data = v;
					match = v == Q(obs_data_get_string(settings_, name));
					break;
				}
				}
				cb->addItem(itemName, data);
				if (obs_property_list_item_disabled(p, i)) {
					cb->setItemData(cb->count() - 1, 0, Qt::UserRole - 1);
				}
				if (match && current < 0)
					current = (int)i;
			}
			if (current >= 0)
				cb->setCurrentIndex(current);
			else if (editable)
				cb->setEditText(Q(obs_data_get_string(settings_, name)));
			else if (fmt == OBS_COMBO_FORMAT_STRING && count > 0 &&
				 !*obs_data_get_string(settings_, name))
				cb->setCurrentIndex(-1);

			auto apply = [this, p, name, cb, fmt] {
				QVariant d = cb->currentData();
				switch (fmt) {
				case OBS_COMBO_FORMAT_INT:
					obs_data_set_int(settings_, name, d.toLongLong());
					break;
				case OBS_COMBO_FORMAT_FLOAT:
					obs_data_set_double(settings_, name, d.toDouble());
					break;
				case OBS_COMBO_FORMAT_BOOL:
					obs_data_set_bool(settings_, name, d.toBool());
					break;
				default: {
					QString s = d.isValid() ? d.toString() : cb->currentText();
					obs_data_set_string(settings_, name, s.toUtf8().constData());
					break;
				}
				}
				Changed(p);
			};
			connect(cb, &QComboBox::activated, this, [apply](int) { apply(); });
			if (editable)
				connect(cb->lineEdit(), &QLineEdit::editingFinished, this, [this, p, name, cb] {
					obs_data_set_string(settings_, name, cb->currentText().toUtf8().constData());
					Changed(p);
				});
			form->addRow(label, cb);
			w = cb;
			break;
		}
		case OBS_PROPERTY_COLOR:
		case OBS_PROPERTY_COLOR_ALPHA: {
			const bool alpha = obs_property_get_type(p) == OBS_PROPERTY_COLOR_ALPHA;
			auto *btn = new QPushButton;
			auto paint = [btn](const QColor &c) {
				btn->setText(c.name(QColor::HexRgb).toUpper());
				btn->setStyleSheet(QStringLiteral("background:%1;color:%2")
							   .arg(c.name(QColor::HexRgb),
								c.lightness() > 128 ? QStringLiteral("#000")
										    : QStringLiteral("#fff")));
			};
			QColor c = FromAbgr(obs_data_get_int(settings_, name));
			if (!alpha)
				c.setAlpha(255);
			paint(c);
			connect(btn, &QPushButton::clicked, this, [this, p, name, alpha, paint] {
				QColor cur = FromAbgr(obs_data_get_int(settings_, name));
				QColor nc = QColorDialog::getColor(cur, this, QString(),
								   alpha ? QColorDialog::ShowAlphaChannel
									 : QColorDialog::ColorDialogOptions());
				if (!nc.isValid())
					return;
				if (!alpha)
					nc.setAlpha(255);
				paint(nc);
				obs_data_set_int(settings_, name, ToAbgr(nc));
				Changed(p);
			});
			form->addRow(label, btn);
			w = btn;
			break;
		}
		case OBS_PROPERTY_FONT: {
			auto *btn = new QPushButton;
			obs_data_t *font = obs_data_get_obj(settings_, name);
			QString face = font ? Q(obs_data_get_string(font, "face")) : QString();
			int size = font ? (int)obs_data_get_int(font, "size") : 36;
			if (font)
				obs_data_release(font);
			btn->setText(QStringLiteral("%1，%2").arg(face.isEmpty() ? QStringLiteral("預設字型") : face).arg(size));
			connect(btn, &QPushButton::clicked, this, [this, p, name, btn] {
				obs_data_t *f = obs_data_get_obj(settings_, name);
				QFont cur(f ? Q(obs_data_get_string(f, "face")) : QString(),
					  f ? std::max(6, (int)obs_data_get_int(f, "size")) : 36);
				if (f)
					obs_data_release(f);
				bool ok = false;
				QFont nf = QFontDialog::getFont(&ok, cur, this);
				if (!ok)
					return;
				obs_data_t *nfData = obs_data_create();
				obs_data_set_string(nfData, "face", nf.family().toUtf8().constData());
				obs_data_set_string(nfData, "style", nf.styleName().toUtf8().constData());
				obs_data_set_int(nfData, "size", nf.pointSize() > 0 ? nf.pointSize() : 36);
				obs_data_set_int(nfData, "flags", (nf.bold() ? 1 : 0) | (nf.italic() ? 2 : 0) |
									  (nf.underline() ? 4 : 0) |
									  (nf.strikeOut() ? 8 : 0));
				obs_data_set_obj(settings_, name, nfData);
				obs_data_release(nfData);
				btn->setText(QStringLiteral("%1，%2").arg(nf.family()).arg(nf.pointSize()));
				Changed(p);
			});
			form->addRow(label, btn);
			w = btn;
			break;
		}
		case OBS_PROPERTY_BUTTON: {
			auto *btn = new QPushButton(label);
			connect(btn, &QPushButton::clicked, this, [this, p] {
				if (obs_property_button_clicked(p, source_))
					QTimer::singleShot(0, this, [this] { Rebuild(); });
			});
			form->addRow(QString(), btn);
			w = btn;
			break;
		}
		case OBS_PROPERTY_GROUP: {
			auto *header = new QLabel(QStringLiteral("<b>%1</b>").arg(label.toHtmlEscaped()));
			if (obs_property_group_type(p) == OBS_GROUP_CHECKABLE) {
				auto *cb = new QCheckBox(label);
				cb->setChecked(obs_data_get_bool(settings_, name));
				connect(cb, &QCheckBox::toggled, this, [this, p, name](bool on) {
					obs_data_set_bool(settings_, name, on);
					Changed(p);
				});
				form->addRow(cb);
				delete header;
			} else {
				form->addRow(header);
			}
			if (obs_properties_t *content = obs_property_group_content(p))
				AddProperties(content, form);
			break;
		}
		default:
			break;
		}

		if (w) {
			w->setEnabled(obs_property_enabled(p));
			const QString tip = Q(obs_property_long_description(p));
			if (!tip.isEmpty())
				w->setToolTip(tip);
		}
	}
}

} // namespace meikou
