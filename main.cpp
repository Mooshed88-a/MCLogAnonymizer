#include <QApplication>
#include <QMainWindow>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextEdit>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QDropEvent>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QMouseEvent>
#include <QDrag>
#include <QUrl>
#include <QSpacerItem>
#include <QProcess>
#include <QDir>
#include <QStringDecoder>


static bool isValidUtf8(const QByteArray &data) {
    const unsigned char *s = reinterpret_cast<const unsigned char*>(data.constData());
    size_t i = 0, len = static_cast<size_t>(data.size());
    while (i < len) {
        unsigned char c = s[i];
        if (c <= 0x7F) { i++; continue; }
        int n = 0;
        if ((c & 0xE0) == 0xC0) { n = 1; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) { n = 2; }
        else if ((c & 0xF8) == 0xF0) { n = 3; if (c > 0xF4) return false; }
        else return false;
        if (i + n >= len) return false;
        for (int k = 1; k <= n; ++k) {
            if ((s[i+k] & 0xC0) != 0x80) return false;
        }
        i += (n + 1);
    }
    return true;
}

static bool looksLikeText(const QByteArray &data) {
    if (data.isEmpty()) return false;
    if (data.contains('\0')) return false;
    int nonPrintable = 0;
    for (unsigned char c : data) {
        if (c == '\n' || c == '\r' || c == '\t') continue;
        if (c < 0x20 || c == 0x7F) nonPrintable++;
    }
    double ratio = static_cast<double>(nonPrintable) / static_cast<double>(data.size());
    return ratio < 0.02;
}

// 文件编码识别，这是我能想到的实现，应该还有更好的方法
static QString decodeBestEffort(const QByteArray &data) {
    if (data.isEmpty()) return {};

    QString utf8Text = QString::fromUtf8(data.constData(), data.size());
    if (isValidUtf8(data)) {
        int validCount = 0;
        for (QChar ch : utf8Text) {
            if (ch.isLetterOrNumber() || ch.isPunct() || ch.isSpace())
                ++validCount;
        }
        double ratio = (double)validCount / utf8Text.size();
        if (ratio > 0.97) {
            return utf8Text;
        }
    }

    return QString::fromLocal8Bit(data.constData(), data.size());
}

static QByteArray runProcessCapture(const QString &program, const QStringList &args, int msecTimeout = 10000, int *exitCodeOut = nullptr) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(2000)) return {};
    if (!p.waitForFinished(msecTimeout)) {
        p.kill(); p.waitForFinished();
        return {};
    }
    if (exitCodeOut) *exitCodeOut = p.exitCode();
    if (p.exitCode() != 0) return {};
    return p.readAllStandardOutput();
}

// 寻找系统可用的解压工具（优先 7z）
static bool hasProgram(const QString &name) {
    int code = -1;
    runProcessCapture(name, { "--help" }, 2000, &code);
    if (code == 0 || code == 1) return true;
#if defined(Q_OS_WIN)
    QStringList candidates = {
        "C:/Program Files/7-Zip/7z.exe",
        "C:/Program Files (x86)/7-Zip/7z.exe"
    };
    for (const auto &c : candidates) {
        int cexit = -1;
        runProcessCapture(c, { "--help" }, 2000, &cexit);
        if (cexit == 0 || cexit == 1) return true;
    }
#endif
    return false;
}

static QString programPathIfExists(const QString &name) {
    int code = -1;
    runProcessCapture(name, { "--help" }, 2000, &code);
    if (code == 0 || code == 1) return name;
#if defined(Q_OS_WIN)
    QStringList candidates = {
        "C:/Program Files/7-Zip/7z.exe",
        "C:/Program Files (x86)/7-Zip/7z.exe"
    };
    for (const auto &c : candidates) {
        int cexit = -1;
        runProcessCapture(c, { "--help" }, 2000, &cexit);
        if (cexit == 0 || cexit == 1) return c;
    }
#endif
    return QString();
}

// 读取普通文本文件
static QString readTextFileAuto(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QByteArray data = f.readAll();
    f.close();
    return decodeBestEffort(data);
}

// 从 .gz 读取
static QByteArray readGzViaTools(const QString &path) {
    QString seven = programPathIfExists("7z");
    if (!seven.isEmpty()) {
        QByteArray out = runProcessCapture(seven, {"x", "-so", path});
        if (!out.isEmpty()) return out;
    }
    QByteArray out = runProcessCapture("gzip", {"-cd", path});
    if (!out.isEmpty()) return out;
#if defined(Q_OS_WIN)
    out = runProcessCapture("tar", {"-xOzf", path});
    if (!out.isEmpty()) return out;
#endif
    return {};
}

// 从 .zip 读取
static QByteArray readZipEntryText(const QString &zipPath, QString *pickedNameOut = nullptr) {
    QString seven = programPathIfExists("7z");
    if (!seven.isEmpty()) {
        QByteArray list = runProcessCapture(seven, {"l", "-ba", zipPath});
        if (!list.isEmpty()) {
            QList<QByteArray> lines = list.split('\n');
            QString picked;
            for (auto &ln : lines) {
                QString s = QString::fromLocal8Bit(ln).trimmed();
                if (s.isEmpty()) continue;
                QString name = s.section(' ', -1);
                if (name.endsWith('/') || name.endsWith('\\')) continue; // 目录
                QString low = name.toLower();
                if (low.endsWith(".log") || low.endsWith(".txt")) { picked = name; break; }
            }
            if (!picked.isEmpty()) {
                if (pickedNameOut) *pickedNameOut = picked;
                QByteArray out = runProcessCapture(seven, {"x", "-so", zipPath, picked});
                return out;
            }
            for (auto &ln : lines) {
                QString s = QString::fromLocal8Bit(ln).trimmed();
                if (s.isEmpty()) continue;
                QString name = s.section(' ', -1);
                if (name.endsWith('/') || name.endsWith('\\')) continue;
                if (pickedNameOut) *pickedNameOut = name;
                QByteArray out = runProcessCapture(seven, {"x", "-so", zipPath, name});
                return out;
            }
        }
    }

    QByteArray list = runProcessCapture("unzip", {"-Z1", zipPath});
    if (!list.isEmpty()) {
        QList<QByteArray> files = list.split('\n');
        QString picked;
        for (auto &fnBA : files) {
            QString name = QString::fromLocal8Bit(fnBA).trimmed();
            if (name.isEmpty()) continue;
            QString low = name.toLower();
            if (low.endsWith(".log") || low.endsWith(".txt")) { picked = name; break; }
        }
        if (picked.isEmpty() && !files.isEmpty()) picked = QString::fromLocal8Bit(files.first()).trimmed();
        if (!picked.isEmpty()) {
            QByteArray out = runProcessCapture("unzip", {"-p", zipPath, picked});
            if (!out.isEmpty()) {
                if (pickedNameOut) *pickedNameOut = picked;
                return out;
            }
        }
    }
    return {};
}

// 从 .tar.gz 读取
static QByteArray readTarGzEntryText(const QString &tgzPath, QString *pickedNameOut = nullptr) {
    QString seven = programPathIfExists("7z");
    if (!seven.isEmpty()) {
        QByteArray list = runProcessCapture(seven, {"l", "-ba", tgzPath});
        if (!list.isEmpty()) {
            QList<QByteArray> lines = list.split('\n');
            QString picked;
            for (auto &ln : lines) {
                QString s = QString::fromLocal8Bit(ln).trimmed();
                if (s.isEmpty()) continue;
                QString name = s.section(' ', -1);
                if (name.endsWith('/') || name.endsWith('\\')) continue;
                QString low = name.toLower();
                if (low.endsWith(".log") || low.endsWith(".txt")) { picked = name; break; }
            }
            if (picked.isEmpty()) {
                for (auto &ln : lines) {
                    QString s = QString::fromLocal8Bit(ln).trimmed();
                    if (s.isEmpty()) continue;
                    QString name = s.section(' ', -1);
                    if (name.endsWith('/') || name.endsWith('\\')) continue;
                    picked = name; break;
                }
            }
            if (!picked.isEmpty()) {
                if (pickedNameOut) *pickedNameOut = picked;
                QByteArray out = runProcessCapture(seven, {"x", "-so", tgzPath, picked});
                if (!out.isEmpty()) return out;
            }
        }
    }

    QByteArray list = runProcessCapture("tar", {"-tzf", tgzPath});
    if (!list.isEmpty()) {
        QList<QByteArray> lines = list.split('\n');
        QString picked;
        for (auto &ln : lines) {
            QString name = QString::fromLocal8Bit(ln).trimmed();
            if (name.isEmpty()) continue;
            if (name.endsWith('/')) continue;
            QString low = name.toLower();
            if (low.endsWith(".log") || low.endsWith(".txt")) { picked = name; break; }
        }
        if (picked.isEmpty()) {
            for (auto &ln : lines) {
                QString name = QString::fromLocal8Bit(ln).trimmed();
                if (name.isEmpty()) continue;
                if (name.endsWith('/')) continue;
                picked = name; break;
            }
        }
        if (!picked.isEmpty()) {
            if (pickedNameOut) *pickedNameOut = picked;
            QByteArray out = runProcessCapture("tar", {"-xOzf", tgzPath, picked});
            if (!out.isEmpty()) return out;
        }
    }
    return {};
}

// ---------------------------- 主窗口 ----------------------------
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("Minecraft 日志脱敏工具");
        resize(750, 700);

        QWidget *central = new QWidget;
        central->setAcceptDrops(true);
        setCentralWidget(central);

        auto *vlay = new QVBoxLayout(central);

        QLabel *lbl = new QLabel(
            "点击【打开文件】选择待处理的日志文件，或将日志文件拖入窗口任意位置\n"
            "点击【脱敏并预览】后将自动删除文件中的所有 IP 地址及端口\n"
            "支持直接打开包含单个文件的 .gz / .zip / .tar.gz 压缩包"
        );
        lbl->setWordWrap(true);
        vlay->addWidget(lbl);

        auto *h1 = new QHBoxLayout;
        fileLabel = new QLabel("未选中文件");
        QPushButton *openBtn = new QPushButton("打开文件");
        anonymizeBtn = new QPushButton("脱敏并预览");
        anonymizeBtn->setEnabled(false);

        qreal scale = this->devicePixelRatioF();
        int minH = int(30 * scale);
        int minW = int(100 * scale);

        QList<QPushButton*> btns = { openBtn, anonymizeBtn };
        for (auto *btn : btns) {
            btn->setMinimumSize(minW, minH);
            QFont f = btn->font();
            f.setPointSizeF(f.pointSizeF() * scale);
            btn->setFont(f);
        }

        h1->addWidget(fileLabel);
        h1->addItem(new QSpacerItem(10,10,QSizePolicy::Expanding,QSizePolicy::Minimum));
        h1->addWidget(openBtn);
        h1->addWidget(anonymizeBtn);
        vlay->addLayout(h1);

        preview = new QTextEdit;
        preview->setReadOnly(true);
        preview->setAcceptDrops(false);
        preview->setPlaceholderText("导入文件后会在此显示原始内容；点击“脱敏并预览”生成脱敏内容。");
        vlay->addWidget(preview);

        auto *h2 = new QHBoxLayout;
        dragExportBtn = new QPushButton("拖拽导出");
        dragExportBtn->setToolTip("可拖拽至文件夹快速导出");
        dragExportBtn->setEnabled(false);

        exportNoteLabel = new QLabel("← 支持拖拽左侧区域至文件夹快速导出");

        saveBtn = new QPushButton("导出文件");
        saveBtn->setEnabled(false);

        QList<QPushButton*> btns2 = { dragExportBtn, saveBtn };
        for (auto *btn : btns2) {
            btn->setMinimumSize(minW, minH);
            QFont f = btn->font();
            f.setPointSizeF(f.pointSizeF() * scale);
            btn->setFont(f);
        }

        h2->addWidget(dragExportBtn);
        h2->addWidget(exportNoteLabel);
        h2->addItem(new QSpacerItem(10,10,QSizePolicy::Expanding,QSizePolicy::Minimum));
        h2->addWidget(saveBtn);
        vlay->addLayout(h2);

        connect(openBtn, &QPushButton::clicked, this, &MainWindow::onOpenFile);
        connect(anonymizeBtn, &QPushButton::clicked, this, &MainWindow::onAnonymize);
        connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveAs);

        dragExportBtn->installEventFilter(this);
        preview->installEventFilter(this);
    }


protected:
    // 接收拖拽
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (event->mimeData()->hasUrls()) {
            const auto urls = event->mimeData()->urls();
            if (!urls.isEmpty() && urls.first().isLocalFile()) {
                event->acceptProposedAction();
            }
        }
    }

    void dropEvent(QDropEvent *event) override {
        if (event->mimeData()->hasUrls()) {
            const auto urls = event->mimeData()->urls();
            if (!urls.isEmpty()) {
                QString local = urls.first().toLocalFile();
                if (!local.isEmpty()) loadFile(local);
            }
            event->acceptProposedAction();
        }
    }

    // 预览区与拖拽控件的拖出导出
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == preview || watched == dragExportBtn) {
            if (event->type() == QEvent::MouseButtonPress) {
                QMouseEvent *me = static_cast<QMouseEvent*>(event);
                lastMousePos = me->pos();
            } else if (event->type() == QEvent::MouseMove) {
                QMouseEvent *me = static_cast<QMouseEvent*>(event);
                if ((me->buttons() & Qt::LeftButton) &&
                    (me->pos() - lastMousePos).manhattanLength() > QApplication::startDragDistance()) {
                    if (anonymizedText.isEmpty()) return false;
                    startDragExport();
                    return true;
                }
            }
        }
        return QMainWindow::eventFilter(watched, event);
    }

private slots:
    void onOpenFile() {
        QString path = QFileDialog::getOpenFileName(this, "选择日志/压缩包文件", QString(),
                                                    "All files (*)");
        if (!path.isEmpty()) loadFile(path);
    }

    void onAnonymize() {
        if (currentDisplayName.isEmpty() || fileContent.isEmpty()) {
            QMessageBox::warning(this, "无可处理内容", "请先导入可识别的文本文件或压缩包。");
            return;
        }
        performAnonymize();
        preview->setPlainText(anonymizedText);
        saveBtn->setEnabled(true);
        dragExportBtn->setEnabled(true);
    }

    void onSaveAs() {
        if (anonymizedText.isEmpty()) {
            QMessageBox::warning(this, "没有脱敏内容", "请先点击“脱敏并预览”生成脱敏内容。");
            return;
        }
        QString suggested = generateAnonymizedFilePath(originalFilePath, currentDisplayName);
        QString path = QFileDialog::getSaveFileName(this, "导出脱敏文件", suggested,
                                                    "Text files (*.txt);;All files (*)");
        if (path.isEmpty()) return;
        if (!exportAnonymizedFile(path)) {
            QMessageBox::critical(this, "保存失败", "无法写入文件：" + path);
            return;
        }
        QMessageBox::information(this, "保存成功", "已导出到：" + path);
    }

private:
    QLabel *fileLabel{nullptr};
    QTextEdit *preview{nullptr};
    QPushButton *dragExportBtn{nullptr};
    QLabel *exportNoteLabel{nullptr};
    QPushButton *saveBtn{nullptr};
    QPushButton *anonymizeBtn{nullptr};

    QString originalFilePath;
    QString currentDisplayName;
    QString fileContent;
    QString anonymizedText;
    QPoint lastMousePos;

    void loadFile(const QString &path) {
        originalFilePath = path;
        QFileInfo fi(path);
        anonymizedText.clear();
        saveBtn->setEnabled(false);
        dragExportBtn->setEnabled(false);
        anonymizeBtn->setEnabled(false);
        fileContent.clear();
        currentDisplayName.clear();

        // 后缀判断（低优先级，真实处理看内容）
        const QString lower = fi.fileName().toLower();

        // 压缩包读取
        if (lower.endsWith(".tar.gz")) {
            QString pickedName;
            QByteArray bytes = readTarGzEntryText(path, &pickedName);
            if (bytes.isEmpty()) {
                preview->setPlainText("无法解压或未找到可读取的日志文件（.log/.txt）。请确认已安装 7z / tar。");
                updateFileLabel(fi, "(tar.gz 读取失败)");
                return;
            }
            QString decoded = decodeBestEffort(bytes);
            if (decoded.isEmpty()) {
                preview->setPlainText("从压缩包中读取到的文件不是可识别的文本或编码识别失败。");
                updateFileLabel(fi, "(tar.gz 非文本)");
                return;
            }
            fileContent = decoded;
            currentDisplayName = pickedName.isEmpty() ? fi.completeBaseName() + ".txt" : pickedName;
            preview->setPlainText(fileContent);
            anonymizeBtn->setEnabled(true);
            updateFileLabel(fi, "(已从 tar.gz 读取)");
            return;
        }
        else if (lower.endsWith(".gz")) {
            QByteArray bytes = readGzViaTools(path);
            if (bytes.isEmpty()) {
                preview->setPlainText("无法解压 .gz，请确认已安装 7z / gzip。");
                updateFileLabel(fi, "(gz 读取失败)");
                return;
            }
            QString decoded = decodeBestEffort(bytes);
            if (decoded.isEmpty()) {
                preview->setPlainText(".gz 内容不是可识别的文本或编码识别失败。");
                updateFileLabel(fi, "(gz 非文本)");
                return;
            }
            fileContent = decoded;
            QString base = fi.fileName();
            if (base.endsWith(".gz", Qt::CaseInsensitive)) base.chop(3);
            currentDisplayName = base.isEmpty() ? fi.completeBaseName() + ".txt" : base;
            preview->setPlainText(fileContent);
            anonymizeBtn->setEnabled(true);
            updateFileLabel(fi, "(已从 gz 读取)");
            return;
        }
        else if (lower.endsWith(".zip")) {
            QString pickedName;
            QByteArray bytes = readZipEntryText(path, &pickedName);
            if (bytes.isEmpty()) {
                preview->setPlainText("无法解压或未找到可读取的日志文件（.log/.txt）。请确认已安装 7z / unzip。");
                updateFileLabel(fi, "(zip 读取失败)");
                return;
            }
            QString decoded = decodeBestEffort(bytes);
            if (decoded.isEmpty()) {
                preview->setPlainText("从 zip 中读取到的文件不是可识别的文本或编码识别失败。");
                updateFileLabel(fi, "(zip 非文本)");
                return;
            }
            fileContent = decoded;
            currentDisplayName = pickedName.isEmpty() ? fi.completeBaseName() + ".txt" : pickedName;
            preview->setPlainText(fileContent);
            anonymizeBtn->setEnabled(true);
            updateFileLabel(fi, "(已从 zip 读取)");
            return;
        }

        QString text = readTextFileAuto(path);
        if (text.isEmpty()) {
            preview->setPlainText("非文本文件或无法识别编码，或不是受支持的压缩包。");
            updateFileLabel(fi, "(非文本/不支持)");
            return;
        }
        fileContent = text;
        currentDisplayName = fi.fileName();
        preview->setPlainText(fileContent);
        anonymizeBtn->setEnabled(true);
        updateFileLabel(fi, "");
    }

    void updateFileLabel(const QFileInfo &fi, const QString &suffix) {
        QString s = fi.fileName() + "    (" + fi.absoluteFilePath() + ")";
        if (!suffix.isEmpty()) s += "  " + suffix;
        fileLabel->setText(s);
    }

    void performAnonymize() {
        const QString octet = "(?:25[0-5]|2[0-4]\\d|1?\\d?\\d)";
        const QString ipPattern = QString("%1\\.%1\\.%1\\.%1").arg(octet);
        const QString ipPortPattern = QString("\\b%1:\\d{1,5}\\b").arg(ipPattern);
        const QString ipOnlyPattern = QString("\\b%1\\b").arg(ipPattern);

        QRegularExpression reIpPort(ipPortPattern);
        QRegularExpression reIp(ipOnlyPattern);

        QString out = fileContent;
        out.replace(reIpPort, "");
        out.replace(reIp, "");
        anonymizedText = out;
    }

    QString generateAnonymizedFilePath(const QString &originPath, const QString &displayName) {
        QFileInfo fin(originPath);
        QFileInfo fdn(displayName);

        QString base = fdn.completeBaseName();
        QString suffix = fdn.suffix();
        if (suffix.isEmpty()) suffix = "txt";

        QString outName = base + "_Anonymized." + suffix;
        QString dir = fin.absoluteDir().absolutePath();
        return dir + QDir::separator() + outName;
    }

    bool exportAnonymizedFile(const QString &targetPath) {
        QFile f(targetPath);
        if (!f.open(QIODevice::WriteOnly)) return false;
        f.write(anonymizedText.toUtf8());
        f.close();
        return true;
    }


    void startDragExport() {
        QString suggested = generateAnonymizedFilePath(originalFilePath, currentDisplayName);
        QFileInfo si(suggested);
        QString tempTarget = QDir::tempPath() + QDir::separator() + si.fileName();

        QFile tf(tempTarget);
        if (!tf.open(QIODevice::WriteOnly)) return;
        tf.write(anonymizedText.toUtf8());
        tf.close();

        QMimeData *mime = new QMimeData;
        mime->setUrls({ QUrl::fromLocalFile(tempTarget) });

        QDrag *drag = new QDrag(dragExportBtn);
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
    }
};

// ---------------------------- 主入口 ----------------------------

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    return a.exec();
}

#include "main.moc"
