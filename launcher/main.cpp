#include "native/k2c_app.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QProcess>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <QCryptographicHash>
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QThread>
#include <QWindow>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QScreen>
#include <QTouchEvent>
#include <QMouseEvent>
extern "C" {
#include "native/k2c_display.h"
}
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <atomic>
#include <zlib.h>

extern "C" int k2c_game_main(void);
extern "C" int k2c_hybris_init(void);

static void configureGameCpus() {
    if(qgetenv("K2C_CPU_MODE")=="all")return;
    cpu_set_t available,preferred;
    CPU_ZERO(&available);CPU_ZERO(&preferred);
    if(sched_getaffinity(0,sizeof available,&available)!=0)return;
    int highest=0,lowest=INT_MAX;
    for(int cpu=0;cpu<CPU_SETSIZE;++cpu) {
        if(!CPU_ISSET(cpu,&available))continue;
        QFile capacity(QString("/sys/devices/system/cpu/cpu%1/cpu_capacity").arg(cpu));
        if(!capacity.open(QIODevice::ReadOnly))continue;
        int value=capacity.readAll().trimmed().toInt();
        if(value<=0)continue;
        lowest=qMin(lowest,value);
        if(value>highest){CPU_ZERO(&preferred);highest=value;}
        if(value==highest)CPU_SET(cpu,&preferred);
    }
    if(highest<=lowest) {
        // Aurora's sandbox hides CPU sysfs. Recognize the verified A55/A76
        // topology through cpuinfo without opening up the sandbox policy.
        QFile info("/proc/cpuinfo");
        if(!info.open(QIODevice::ReadOnly))return;
        CPU_ZERO(&preferred);
        bool hasLittle=false;
        int cpu=-1,implementer=0;
        for(const QByteArray &line:info.readAll().split('\n')) {
            const int colon=line.indexOf(':');
            if(colon<0)continue;
            const QByteArray key=line.left(colon).trimmed();
            const QByteArray value=line.mid(colon+1).trimmed();
            if(key=="processor") { cpu=value.toInt();implementer=0; }
            else if(key=="CPU implementer")implementer=value.toInt(nullptr,0);
            else if(key=="CPU part" && implementer==0x41 && cpu>=0 && cpu<CPU_SETSIZE && CPU_ISSET(cpu,&available)) {
                int part=value.toInt(nullptr,0);
                if(part==0xd05)hasLittle=true;
                else if(part==0xd0b)CPU_SET(cpu,&preferred);
            }
        }
        if(!hasLittle || CPU_COUNT(&preferred)==0)return;
    }
    // Existing booster/Qt threads also need the same mask; future threads inherit it.
    int changed=0;
    for(const QString &tid:QDir("/proc/self/task").entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
        bool ok=false;int id=tid.toInt(&ok);
        if(ok && sched_setaffinity(id,sizeof preferred,&preferred)==0)++changed;
    }
    QStringList ids;
    for(int cpu=0;cpu<CPU_SETSIZE;++cpu)if(CPU_ISSET(cpu,&preferred))ids<<QString::number(cpu);
    fprintf(stderr,"[performance] preferred CPUs=%s, affinity applied to %d threads\n",qPrintable(ids.join(",")),changed);
}

// Keep the Aurora application's event loop and lifecycle connection alive during play.
class GameThread : public QThread {
public:
    int result = 1;
    void run() override { result = k2c_hybris_init() ? k2c_game_main() : 1; }
};

// Render-thread owned texture. Storage is allocated only on first frame/resize.
class FrameTexture : public QSGTexture {
    GLuint id=0;
    QSize size;
    bool allocated=false;
    QOpenGLFunctions *gl;
public:
    FrameTexture(const QSize &initialSize) : size(initialSize), gl(QOpenGLContext::currentContext()->functions()) {
        gl->glGenTextures(1,&id);
        setFiltering(QSGTexture::Nearest);
        setHorizontalWrapMode(QSGTexture::ClampToEdge);
        setVerticalWrapMode(QSGTexture::ClampToEdge);
    }
    ~FrameTexture() override { if(id)gl->glDeleteTextures(1,&id); }
    int textureId() const override { return id; }
    QSize textureSize() const override { return size; }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }
    void bind() override { gl->glBindTexture(GL_TEXTURE_2D,id);updateBindOptions(); }
    void upload(const QImage &image) {
        GLint previous=0,alignment=0,rowLength=0;
        gl->glGetIntegerv(GL_TEXTURE_BINDING_2D,&previous);
        gl->glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);
        gl->glGetIntegerv(0x0cf2,&rowLength); // GLES3 GL_UNPACK_ROW_LENGTH
        gl->glBindTexture(GL_TEXTURE_2D,id);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT,4);
        gl->glPixelStorei(0x0cf2,0);
        if(!allocated || size!=image.size()) {
            size=image.size();
            allocated=true;
            gl->glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,size.width(),size.height(),0,GL_RGBA,GL_UNSIGNED_BYTE,image.constBits());
            gl->glTexParameteri(GL_TEXTURE_2D,0x8e45,GL_ONE); // GL_TEXTURE_SWIZZLE_A: opaque Android frame
            updateBindOptions(true);
            fprintf(stderr,"[display] GLES texture allocated %dx%d, GL=%s\n",size.width(),size.height(),gl->glGetString(GL_VERSION));
        } else gl->glTexSubImage2D(GL_TEXTURE_2D,0,0,0,size.width(),size.height(),GL_RGBA,GL_UNSIGNED_BYTE,image.constBits());
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);
        gl->glPixelStorei(0x0cf2,rowLength);
        gl->glBindTexture(GL_TEXTURE_2D,previous);
    }
};

class FrameNode : public QSGSimpleTextureNode {
public:
    FrameTexture *frameTexture;
    QImage pixels;
    FrameNode(const QSize &initialSize) : frameTexture(new FrameTexture(initialSize)) {
        setOwnsTexture(true);
        setTexture(frameTexture);
        setFiltering(QSGTexture::Nearest);
    }
};

class DisplayKeeper : public QObject {
    QTimer timer;
    bool enabled=false;
    void send(const QString &method) {
        QDBusMessage message=QDBusMessage::createMethodCall("com.nokia.mce","/com/nokia/mce/request","com.nokia.mce.request",method);
        message.setAutoStartService(false);
        if(!QDBusConnection::systemBus().send(message))
            fprintf(stderr,"[display] MCE request unavailable\n");
    }
public:
    DisplayKeeper() {
        timer.setInterval(55000);
        connect(&timer,&QTimer::timeout,this,[this]{send("req_display_blanking_pause");});
    }
    ~DisplayKeeper() { setEnabled(false); }
    void setEnabled(bool value) {
        if(value==enabled)return;
        enabled=value;
        if(enabled){send("req_display_blanking_pause");timer.start();}
        else {timer.stop();send("req_display_cancel_blanking_pause");}
        fprintf(stderr,"[display] keep awake=%d\n",enabled);
    }
};

class GameView : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameReady)
    std::atomic<bool> ready{false};
    QMutex frameMutex;
    QImage frame;
    bool dirty = false;
    int contact = -1;
    QElapsedTimer clock;
    qint64 lastReport=0;
    unsigned long displayed=0,lastDisplayed=0;
    void sendInput(int action, const QPointF &point) {
        if(width()>0 && height()>0)
            k2c_display_host_input(action, point.x()/width(), point.y()/height());
    }
signals:
    void frameReady();
public slots:
    void notifyReady() { emit frameReady(); }
public:
    bool hasFrame() const { return ready.load(); }
    GameView(QQuickItem *parent=nullptr) : QQuickItem(parent) {
        clock.start();
        setAcceptedMouseButtons(Qt::LeftButton);
        setFlag(ItemHasContents,true);
        QTimer *timer=new QTimer(this);
        connect(timer,&QTimer::timeout,this,[this]{
            bool refresh;
            { QMutexLocker lock(&frameMutex); refresh=dirty; }
            if(refresh)update();
        });
        timer->start(16);
    }
    ~GameView() override { k2c_display_set_host(0,0,nullptr,nullptr); }
    static void receiveFrame(void *context,const void *pixels,int width,int height,int stride) {
        GameView *view=static_cast<GameView*>(context);
        QMutexLocker lock(&view->frameMutex);
        if(view->frame.size()!=QSize(width,height))
            view->frame=QImage(width,height,QImage::Format_RGBX8888);
        if(view->frame.isNull())return;
        for(int y=0;y<height;++y)
            memcpy(view->frame.scanLine(y),static_cast<const char*>(pixels)+y*stride,width*4);
        view->dirty=true;
    }
protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override {
        FrameNode *node=static_cast<FrameNode*>(oldNode);
        bool refresh=false;
        {
            QMutexLocker lock(&frameMutex);
            if(!node && !frame.isNull()) { node=new FrameNode(frame.size());dirty=true; }
            if(!node)return nullptr;
            if(dirty) { frame.swap(node->pixels);dirty=false;refresh=true; }
        }
        if(refresh) {
            node->frameTexture->upload(node->pixels);
            node->markDirty(QSGNode::DirtyMaterial);
            ++displayed;
            const qint64 now=clock.nsecsElapsed();
            if(now-lastReport>=5000000000LL) {
                fprintf(stderr,"[performance] Qt %.1f fps, texture %dx%d\n",
                    (displayed-lastDisplayed)*1e9/(now-lastReport),node->pixels.width(),node->pixels.height());
                lastReport=now;lastDisplayed=displayed;
            }
            if(!ready) { ready=true;QMetaObject::invokeMethod(this,"notifyReady",Qt::QueuedConnection); }
        }
        node->setRect(boundingRect());
        return node;
    }
    void touchEvent(QTouchEvent *event) override {
        if(event->type()==QEvent::TouchCancel) {
            if(contact>=0)k2c_display_host_input(3,0,0);
            contact=-1; event->accept(); return;
        }
        for(const QTouchEvent::TouchPoint &point:event->touchPoints()) {
            if(contact<0 && point.state()==Qt::TouchPointPressed) {
                contact=point.id(); sendInput(0,point.pos());
            } else if(point.id()==contact) {
                if(point.state()==Qt::TouchPointReleased) { sendInput(1,point.pos()); contact=-1; }
                else if(point.state()==Qt::TouchPointMoved)sendInput(2,point.pos());
            }
        }
        event->accept();
    }
    void mousePressEvent(QMouseEvent *event) override { sendInput(0,event->localPos());event->accept(); }
    void mouseMoveEvent(QMouseEvent *event) override { sendInput(2,event->localPos());event->accept(); }
    void mouseReleaseEvent(QMouseEvent *event) override { sendInput(1,event->localPos());event->accept(); }
    void touchUngrabEvent() override { contact=-1;k2c_display_host_input(3,0,0); }
    void mouseUngrabEvent() override { contact=-1;k2c_display_host_input(3,0,0); }
};

struct ZipEntry { QString name; quint16 method; quint32 compressed; quint32 uncompressed; quint32 offset; quint32 crc; };
static quint16 zip16(const QByteArray &b, int p) { return quint16(uchar(b[p])) | (quint16(uchar(b[p+1])) << 8); }
static quint32 zip32(const QByteArray &b, int p) { return quint32(zip16(b,p)) | (quint32(zip16(b,p+2)) << 16); }

static bool findZipEntry(QFile &zip, const QString &wanted, ZipEntry &result) {
    if (!zip.open(QIODevice::ReadOnly)) return false;
    const qint64 tailSize=qMin<qint64>(zip.size(),65557);
    if (!zip.seek(zip.size()-tailSize)) return false;
    QByteArray tail=zip.read(tailSize);
    int end=-1;
    for(int i=tail.size()-22;i>=0;--i) if(zip32(tail,i)==0x06054b50u){end=i;break;}
    if(end<0) return false;
    const quint16 count=zip16(tail,end+10);
    const quint32 cdOffset=zip32(tail,end+16);
    if(!zip.seek(cdOffset)) return false;
    for(quint32 i=0;i<count;++i){
        QByteArray h=zip.read(46); if(h.size()!=46 || zip32(h,0)!=0x02014b50u) return false;
        const quint16 nameLen=zip16(h,28), extraLen=zip16(h,30), commentLen=zip16(h,32);
        QByteArray name=zip.read(nameLen); if(name.size()!=nameLen) return false;
        ZipEntry e; e.name=QString::fromUtf8(name); e.method=zip16(h,10); e.crc=zip32(h,16);
        e.compressed=zip32(h,20); e.uncompressed=zip32(h,24); e.offset=zip32(h,42);
        if(e.name==wanted){ result=e; return true; }
        if(!zip.seek(zip.pos()+extraLen+commentLen)) return false;
    }
    return false;
}

template<class Sink> static bool streamZipEntry(QFile &zip,const ZipEntry &e,Sink sink){
    if(!zip.seek(e.offset)) return false;
    QByteArray h=zip.read(30); if(h.size()!=30 || zip32(h,0)!=0x04034b50u) return false;
    const quint16 nameLen=zip16(h,26),extraLen=zip16(h,28);
    if(!zip.seek(zip.pos()+nameLen+extraLen)) return false;
    quint32 remaining=e.compressed, crc=::crc32(0L,Z_NULL,0); quint64 total=0;
    z_stream zs; memset(&zs,0,sizeof(zs));
    if(e.method==8 && inflateInit2(&zs,-MAX_WBITS)!=Z_OK) return false;
    if(e.method!=0 && e.method!=8) return false;
    char in[65536],out[65536]; bool ok=true;
    while(remaining && ok){
        const int n=int(qMin<quint32>(remaining,sizeof(in))); QByteArray chunk=zip.read(n);
        if(chunk.size()!=n){ok=false;break;} remaining-=n;
        if(e.method==0){crc=::crc32(crc,reinterpret_cast<const Bytef*>(chunk.constData()),n);total+=n;ok=sink(chunk.constData(),n);continue;}
        zs.next_in=reinterpret_cast<Bytef*>(chunk.data());zs.avail_in=n;
        while(zs.avail_in && ok){zs.next_out=reinterpret_cast<Bytef*>(out);zs.avail_out=sizeof(out);
            const int ret=inflate(&zs,Z_NO_FLUSH);const int made=sizeof(out)-zs.avail_out;
            if(ret!=Z_OK && ret!=Z_STREAM_END){ok=false;break;}
            if(made){crc=::crc32(crc,reinterpret_cast<const Bytef*>(out),made);total+=made;ok=sink(out,made);}
            if(ret==Z_STREAM_END) break;
        }
    }
    if(e.method==8) inflateEnd(&zs);
    return ok && total==e.uncompressed && crc==e.crc;
}

static bool compatibleApk(const QString &path){
    QFile zip(path);ZipEntry e;
    if(!findZipEntry(zip,"lib/arm64-v8a/libunity.so",e))return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return streamZipEntry(zip,e,[&](const char *p,int n){hash.addData(p,n);return true;}) &&
        hash.result().toHex()==QByteArray("2de032fd5d52b0a6cd684c1b13b5dc8739b0035bc9e07baeb36e16173a923292");
}

static bool extractApkAssets(const QString &path,const QString &destination){
    QFile zip(path); if(!zip.open(QIODevice::ReadOnly))return false;
    const qint64 tailSize=qMin<qint64>(zip.size(),65557); if(!zip.seek(zip.size()-tailSize))return false;
    QByteArray tail=zip.read(tailSize);int end=-1;
    for(int i=tail.size()-22;i>=0;--i)if(zip32(tail,i)==0x06054b50u){end=i;break;}
    if(end<0||!zip.seek(zip32(tail,end+16)))return false;
    const quint16 count=zip16(tail,end+10);QDir().mkpath(destination);
    for(quint32 i=0;i<count;++i){
        QByteArray h=zip.read(46);if(h.size()!=46||zip32(h,0)!=0x02014b50u)return false;
        const quint16 nl=zip16(h,28),xl=zip16(h,30),cl=zip16(h,32);QByteArray raw=zip.read(nl);if(raw.size()!=nl)return false;
        const qint64 nextCentral=zip.pos()+xl+cl;
        QString name=QString::fromUtf8(raw);ZipEntry e;e.name=name;e.method=zip16(h,10);e.crc=zip32(h,16);e.compressed=zip32(h,20);e.uncompressed=zip32(h,24);e.offset=zip32(h,42);
        if(name.startsWith("assets/")&&!name.contains("../")){
            QString relative=name;QString output=QDir(destination).filePath(relative);
            if(name.endsWith('/')){if(!QDir().mkpath(output))return false;}
            else {QFileInfo info(output);if(!QDir().mkpath(info.absolutePath()))return false;
                QFile out(output);if(!out.open(QIODevice::WriteOnly|QIODevice::Truncate))return false;
                bool ok=streamZipEntry(zip,e,[&](const char *p,int n){return out.write(p,n)==n;});out.close();if(!ok)return false;
            }
        }
        if(!zip.seek(nextCentral))return false;
    }
    return true;
}

// QSettings::sync can take seconds on the device filesystem. Keep it off the UI thread.
class SettingsWriter : public QThread {
    Q_OBJECT
    QString filename;
    QMutex mutex;
    QWaitCondition wake;
    QVariantMap pending;
    bool stopping=false;
public:
    explicit SettingsWriter(const QString &path) : filename(path) { start(); }
    ~SettingsWriter() override {
        { QMutexLocker lock(&mutex); stopping=true; wake.wakeOne(); }
        wait(); // Drain the final selection before normal application exit.
    }
    void save(const QVariantMap &values) {
        QMutexLocker lock(&mutex);
        pending=values; // Coalesce rapid changes; the latest complete snapshot wins.
        wake.wakeOne();
    }
signals:
    void writeFailed();
protected:
    void run() override {
        for(;;) {
            QVariantMap values;
            {
                QMutexLocker lock(&mutex);
                while(pending.isEmpty() && !stopping) wake.wait(&mutex);
                if(pending.isEmpty() && stopping) return;
                values=pending; pending.clear();
            }
            QElapsedTimer elapsed; elapsed.start();
            QSettings settings(filename,QSettings::IniFormat);
            for(auto it=values.constBegin();it!=values.constEnd();++it)
                settings.setValue(it.key(),it.value());
            settings.sync();
            fprintf(stderr,"[launcher] background settings sync: %lld ms\n",static_cast<long long>(elapsed.elapsed()));
            if(settings.status()!=QSettings::NoError) emit writeFailed();
        }
    }
};

class Launcher : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString folder READ folder NOTIFY folderChanged)
    Q_PROPERTY(QString selected READ selected NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY folderChanged)
    Q_PROPERTY(bool skip READ skip WRITE setSkip NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    QString path = QDir::homePath()+"/Downloads", apk, status;
    bool bypass = false, busy = false, playing = false;
    QString cacheKey, savedCacheKey;
#ifdef K2C_BUILTIN_GAME
    SettingsWriter settingsWriter{QDir::homePath()+"/.config/" K2C_APP_STORAGE "/builtin-settings.ini"};
#else
    SettingsWriter settingsWriter{QDir::homePath()+"/.config/" K2C_APP_STORAGE "/settings.ini"};
#endif
    QProcess unzip;
public:
    Launcher() {
        connect(&settingsWriter,&SettingsWriter::writeFailed,this,[this]{status="Не удалось сохранить настройки лаунчера.";emit changed();});
#ifdef K2C_BUILTIN_GAME
        apk="/usr/share/" K2C_APP_ID "/game/base.apk";
        bypass=true;
#else
        QSettings settings(QDir::homePath()+"/.config/" K2C_APP_STORAGE "/settings.ini",QSettings::IniFormat);
        apk=settings.value("apk").toString(); bypass=settings.value("skip",false).toBool();
        savedCacheKey=settings.value("cacheKey").toString();
        if (!apk.isEmpty() && QFileInfo(apk).isReadable()) path=QFileInfo(apk).absolutePath(); else apk.clear();
#endif
        connect(&unzip, static_cast<void(QProcess::*)(int,QProcess::ExitStatus)>(&QProcess::finished),this,[this](int code,QProcess::ExitStatus exit){
            busy=false;
            if(code || exit!=QProcess::NormalExit) { status="Не удалось распаковать APK: "+QString::fromLocal8Bit(unzip.readAllStandardError()).left(250); emit changed(); return; }
            QDir cache(QDir::homePath()+"/.local/share/" K2C_APP_STORAGE "/apk-data");
            if(!QFileInfo(cache.filePath("assets/bin/Data/globalgamemanagers")).isReadable()) {status="В APK нет данных игры."; emit changed(); return;}
            savedCacheKey=cacheKey; persistSelection();
            startGame(cache.filePath("assets"));
        });
        connect(&unzip,&QProcess::errorOccurred,this,[this](QProcess::ProcessError){busy=false;status="Не удалось запустить распаковку APK.";emit changed();});
    }
    bool running()const{return playing;}
    QString folder()const{return path;} QString selected()const{return apk;} QString message()const{return status;} bool skip()const{return bypass;}
    void persistSelection() {
        QVariantMap values; values["apk"]=apk; values["skip"]=bypass;
        values["cacheKey"]=savedCacheKey;
        settingsWriter.save(values);
    }
    void setSkip(bool value){bypass=value;persistSelection();emit changed();}
    QVariantList entries()const {
        QVariantList list;
        for(const QFileInfo &f:QDir(path).entryInfoList(QDir::AllDirs|QDir::Files|QDir::NoDotAndDotDot,QDir::DirsFirst|QDir::Name)) {
            if(!f.isDir() && f.suffix().compare("apk",Qt::CaseInsensitive))continue;
            QVariantMap row;row["name"]=f.fileName();row["path"]=f.absoluteFilePath();row["directory"]=f.isDir();list<<row;
        } return list;
    }
    Q_INVOKABLE void choose(QString value){if(busy)return;if(QFileInfo(value).isDir()){path=value;emit folderChanged();}else {apk=value;persistSelection();}emit changed();}
    Q_INVOKABLE void up(){if(busy)return;QDir d(path);d.cdUp();path=d.absolutePath();emit folderChanged();emit changed();}
    Q_INVOKABLE void launch(){
        if(busy || playing)return;
        if(!QFileInfo(apk).isFile()||!QFileInfo(apk).isReadable()){status="Выберите доступный APK игры.";emit changed();return;}
        cacheKey=apk+QString::number(QFileInfo(apk).size())+QFileInfo(apk).lastModified().toString(Qt::ISODate);
        if(savedCacheKey==cacheKey && QFileInfo(QDir::homePath()+"/.local/share/" K2C_APP_STORAGE "/apk-data/assets/bin/Data/globalgamemanagers").isReadable()){persistSelection();startGame(QDir::homePath()+"/.local/share/" K2C_APP_STORAGE "/apk-data/assets");return;}
        status="Проверка APK…";emit changed();
        if(!compatibleApk(apk)){status="Этот APK не совместим с текущей сборкой игры.";emit changed();return;}
        QDir cache(QDir::homePath()+"/.local/share/" K2C_APP_STORAGE "/apk-data");
        if(cache.exists()&&!cache.removeRecursively()){status="Не удалось обновить данные APK.";emit changed();return;}
        QDir().mkpath(cache.absolutePath());busy=true;status="Подготовка игровых данных…";emit changed();
        unzip.start("/usr/bin/" K2C_APP_ID "",{"--extract-assets",apk,cache.absolutePath()});
    }
    void autoLaunch(){if(bypass && QFileInfo(apk).isFile() && QFileInfo(apk).isReadable())launch();}
    void gameError(){status="Не удалось запустить игровой движок. Подробности в launcher.log.";emit changed();}
    static bool copyTree(const QString &source, const QString &destination) {
        QDir src(source);
        if (!src.exists()) return false;
        QDir().mkpath(destination);
        for (const QFileInfo &entry : src.entryInfoList(QDir::AllEntries|QDir::NoDotAndDotDot)) {
            const QString to=destination+"/"+entry.fileName();
            if (entry.isDir()) { if(!copyTree(entry.absoluteFilePath(),to)) return false; }
            else if(!QFile::exists(to) && !QFile::copy(entry.absoluteFilePath(),to)) return false;
        }
        return true;
    }
    static bool migrateLegacyStorage() {
        const QString home=QDir::homePath();
        const QString config=home+"/.config/" K2C_APP_STORAGE;
        const QString data=home+"/.local/share/" K2C_APP_STORAGE;
        if(!QDir().mkpath(config) || !QDir().mkpath(data)) return false;
        const QString marker=config+"/.storage-migrated";
        if(QFileInfo::exists(marker)) return true;
        const QString oldConfig=home+"/.config/" K2C_APP_ID;
        const QString oldData=home+"/.local/share/" K2C_APP_ID;
        if(QDir(oldConfig).exists() && !copyTree(oldConfig,config)) return false;
        // Copy durable game state without importing executables or shader caches.
        for(const QString &part: {QString("external"),QString("files"),QString("data")}) {
            const QString source=oldData+"/game/"+part;
            if(QDir(source).exists() && !copyTree(source,data+"/game/"+part)) return false;
        }
        QFile file(marker);
        return file.open(QIODevice::WriteOnly) && file.write("1\n")==2;
    }
    static bool initializeGamePreferences(const QString &gameData) {
        const QString directory=gameData+"/external/Release";
        const QString filename=directory+"/global-v35";
        if(QFileInfo::exists(filename)) return true;
        if(!QDir().mkpath(directory)) return false;
        // Fresh save envelope: no campaign, purchases, analytics or user data.
        const QByteArray defaults=R"({"serializedSaveDataVersion":16,"campaigns":[],"challenges":[],"challengeRanks":[],"cursedChallengesCompleted":[],"_currentCampaign":0,"_currentChallenge":0,"currentChallengePlayedInMultiplayer":false,"_hasCompletedTutorial":false,"LastSeenDLCPromptVersion":0,"_timePlayedGlobal":0,"_timePlayedCampaigns":[0,0,0,0,0,0,0,0,0,0],"_eventCrownData":{"UnlockedEventCrowns":[]},"prefs":{"srzEntries":[{"key":"PreferredZoom","val":"2"},{"key":"DidChangeFromSystemLanguage","val":"1"},{"key":"Language","val":"Russian"}]},"persistentInGameTimers":[]})";
        const QString temporary=filename+".tmp";
        gzFile output=gzopen(temporary.toLocal8Bit().constData(),"wb");
        if(!output) return false;
        const int written=gzwrite(output,defaults.constData(),defaults.size());
        const int result=gzclose(output);
        if(written!=defaults.size() || result!=Z_OK || !QFile::rename(temporary,filename)) {
            QFile::remove(temporary); return false;
        }
        return true;
    }
    void startGame(const QString &assets){
        const QString runtime="/usr/share/" K2C_APP_ID "/runtime";
        const QString gameData=QDir::homePath()+"/.local/share/" K2C_APP_STORAGE "/game";
        QDir().mkpath(gameData+"/files");
        QDir().mkpath(gameData+"/cache");
        QDir().mkpath(gameData+"/data");
        QDir().mkpath(gameData+"/obb");
        QDir().mkpath(gameData+"/external");
        copyTree(runtime+"/external",gameData+"/external");
        copyTree(runtime+"/files",gameData+"/files");
        if(!initializeGamePreferences(gameData)) {status="Не удалось сохранить начальные настройки игры.";emit changed();return;}
        QProcessEnvironment env=QProcessEnvironment::systemEnvironment();
        env.insert("K2C_ASSETS_ROOT",assets);
        env.insert("K2C_APK_PATH",apk);
        env.insert("K2C_DATA_ROOT",QFileInfo(assets).absolutePath());
        env.insert("K2C_LIB_ROOT",runtime+"/lib/arm64-v8a");
        env.insert("K2C_GAME_DATA_ROOT",gameData);
        qputenv("K2C_ASSETS_ROOT",assets.toLocal8Bit());
        qputenv("K2C_APK_PATH",apk.toLocal8Bit());
        qputenv("K2C_DATA_ROOT",QFileInfo(assets).absolutePath().toLocal8Bit());
        qputenv("K2C_LIB_ROOT",(runtime+"/lib/arm64-v8a").toLocal8Bit());
        qputenv("K2C_GAME_DATA_ROOT",gameData.toLocal8Bit());
        const QByteArray logPath=(gameData+"/launcher.log").toLocal8Bit();
        int logFd=qEnvironmentVariableIsSet("K2C_DEBUG_STDIO") ? -1 : open(logPath.constData(),O_WRONLY|O_CREAT|O_APPEND,0644);
        if(logFd>=0){dup2(logFd,STDOUT_FILENO);dup2(logFd,STDERR_FILENO);if(logFd>STDERR_FILENO)close(logFd);}
        if(chdir(gameData.toLocal8Bit().constData())!=0){status="Не удалось открыть каталог игры.";emit changed();return;}
        GameView *view=nullptr;
        for(QWindow *window : QGuiApplication::topLevelWindows()) {
            view=window->findChild<GameView*>();
            if(view)break;
        }
        if(!view){status="Не удалось создать экран игры.";emit changed();return;}
        QSize screen=QGuiApplication::primaryScreen()->size();
        k2c_display_set_host(qMax(screen.width(),screen.height()),qMin(screen.width(),screen.height()),GameView::receiveFrame,view);
        playing=true; emit changed();
        configureGameCpus();
        GameThread *game=new GameThread;
        connect(game,&QThread::finished,this,[this,game]{
            playing=false;
            if(game->result) {
                status="Не удалось запустить игровой движок. Подробности в launcher.log.";
                for(QWindow *window : QGuiApplication::topLevelWindows()) window->show();
                emit changed();
            } else QCoreApplication::quit();
            game->deleteLater();
        });
        game->start();
    }
 signals:void changed(); void folderChanged();
};
__attribute__((visibility("default"))) int main(int argc,char **argv){
    if(argc==2 && strcmp(argv[1],"--run-game")==0) {
        const int result=k2c_hybris_init() ? k2c_game_main() : 1;
        if(!result)return 0;
        char *const args[]={argv[0],const_cast<char*>("--game-error"),nullptr};
        execv("/usr/bin/" K2C_APP_ID "",args);
        return result;
    }
    if(argc==4 && strcmp(argv[1],"--extract-assets")==0)
        return extractApkAssets(QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3])) ? 0 : 2;
    if(!qEnvironmentVariableIsSet("XDG_RUNTIME_DIR"))qputenv("XDG_RUNTIME_DIR","/run/display");
    if(!qEnvironmentVariableIsSet("WAYLAND_DISPLAY"))qputenv("WAYLAND_DISPLAY","/run/display/wayland-0");
    configureGameCpus();
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setVersion(3,0);
    format.setSamples(0);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc,argv);qmlRegisterType<GameView>("Kingdom",1,0,"GameView");QCoreApplication::setApplicationName("" K2C_APP_ID "");QCoreApplication::setOrganizationName("ru.nighteugene");QDir().mkpath(QDir::homePath()+"/.config/" K2C_APP_STORAGE "");QDir().mkpath(QDir::homePath()+"/.local/share/" K2C_APP_STORAGE "");if(!Launcher::migrateLegacyStorage()){fprintf(stderr,"Cannot prepare persistent Kingdom storage\n");return 1;}Launcher launcher;QQmlApplicationEngine engine;engine.rootContext()->setContextProperty("launcher",&launcher);
#ifdef K2C_BUILTIN_GAME
    engine.load(QUrl("qrc:/Game.qml"));
#else
    engine.load(QUrl("qrc:/Launcher.qml"));
#endif
    if(engine.rootObjects().isEmpty())return 1;
    DisplayKeeper displayKeeper;
    auto refreshDisplay=[&]{displayKeeper.setEnabled(launcher.running() && QGuiApplication::applicationState()==Qt::ApplicationActive);};
    QObject::connect(&launcher,&Launcher::changed,&displayKeeper,refreshDisplay);
    QObject::connect(&app,&QGuiApplication::applicationStateChanged,&displayKeeper,[&](Qt::ApplicationState){refreshDisplay();});
    QObject::connect(&app,&QCoreApplication::aboutToQuit,&displayKeeper,[&]{displayKeeper.setEnabled(false);});
    if(argc==2 && strcmp(argv[1],"--game-error")==0)launcher.gameError();
#ifdef K2C_BUILTIN_GAME
    else QTimer::singleShot(0,&launcher,[&launcher]{launcher.launch();});
#else
    else if(argc==2 && strcmp(argv[1],"--launch")==0)
        QTimer::singleShot(0,&launcher,[&launcher]{launcher.launch();});
    else QTimer::singleShot(0,&launcher,[&launcher]{launcher.autoLaunch();});
#endif
    return app.exec();
}
#include "main.moc"
