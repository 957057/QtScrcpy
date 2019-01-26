#include <QDesktopWidget>
#include <QMouseEvent>
#include <QTimer>
#include <QStyle>
#include <QStyleOption>
#include <QPainter>
#ifdef Q_OS_WIN32
#include <Windows.h>
#endif
#include <QQuickWidget>

#include "videoform.h"
#include "ui_videoform.h"
#include "iconhelper.h"
#include "toolform.h"
#include "controlevent.h"

VideoForm::VideoForm(const QString& serial, quint16 maxSize, quint32 bitRate,QWidget *parent) :
    QWidget(parent),
    ui(new Ui::videoForm),
    m_serial(serial),
    m_maxSize(maxSize),
    m_bitRate(bitRate)
{    
    ui->setupUi(this);    
    initUI();

    connect(&m_inputConvert, &InputConvertGame::grabCursor, this, [this](bool grab){
#ifdef Q_OS_WIN32
        if(grab) {
            QRect rc(mapToGlobal(ui->videoWidget->pos())
                     , ui->videoWidget->size());
            RECT mainRect;
            mainRect.left = (LONG)rc.left();
            mainRect.right = (LONG)rc.right();
            mainRect.top = (LONG)rc.top();
            mainRect.bottom = (LONG)rc.bottom();
            ClipCursor(&mainRect);
        } else {
            ClipCursor(Q_NULLPTR);
        }
#endif
    });

    m_server = new Server();
    m_frames.init();
    m_decoder.setFrames(&m_frames);

    connect(m_server, &Server::serverStartResult, this, [this](bool success){
        if (success) {
            m_server->connectTo();            
        }
    });

    connect(m_server, &Server::connectToResult, this, [this](bool success, const QString &deviceName, const QSize &size){
        if (success) {
            // update ui
            setWindowTitle(deviceName);
            updateShowSize(size);            

            // init decode
            m_decoder.setDeviceSocket(m_server->getDeviceSocket());
            m_decoder.startDecode();

            // init controller
            m_inputConvert.setDeviceSocket(m_server->getDeviceSocket());
        }
    });

    connect(m_server, &Server::onServerStop, this, [this](){
        close();
        qDebug() << "server process stop";
    });

    connect(&m_decoder, &Decoder::onDecodeStop, this, [this](){
        close();
        qDebug() << "decoder thread stop";
    });

    // must be Qt::QueuedConnection, ui update must be main thread
    QObject::connect(&m_decoder, &Decoder::onNewFrame, this, [this](){
        if (ui->videoWidget->isHidden()) {
            ui->loadingWidget->close();
            ui->videoWidget->show();
        }
        m_frames.lock();        
        const AVFrame *frame = m_frames.consumeRenderedFrame();
        //qDebug() << "widthxheight:" << frame->width << "x" << frame->height;
        updateShowSize(QSize(frame->width, frame->height));
        ui->videoWidget->setFrameSize(QSize(frame->width, frame->height));
        ui->videoWidget->updateTextures(frame->data[0], frame->data[1], frame->data[2], frame->linesize[0], frame->linesize[1], frame->linesize[2]);
        m_frames.unLock();
    },Qt::QueuedConnection);

    // fix: macos cant recv finished signel, timer is ok
    QTimer::singleShot(0, this, [this](){
        // max size support 480p 720p 1080p 设备原生分辨率
        // support wireless connect, example:
        //m_server->start("192.168.0.174:5555", 27183, m_maxSize, m_bitRate, "");
        // only one devices, serial can be null
        m_server->start(m_serial, 27183, m_maxSize, m_bitRate, "");
    });

    updateShowSize(size());

    bool vertical = size().height() > size().width();
    updateStyleSheet(vertical);
}

VideoForm::~VideoForm()
{
    m_server->stop();
    m_decoder.stopDecode();
    delete m_server;
    m_frames.deInit();
    delete ui;
}

void VideoForm::initUI()
{
    QPixmap phone;
    if (phone.load(":/res/phone.png")) {
        m_widthHeightRatio = 1.0f * phone.width() / phone.height();
    }

    setAttribute(Qt::WA_DeleteOnClose);
    // 去掉标题栏
    setWindowFlags(Qt::FramelessWindowHint);    
    // 根据图片构造异形窗口
    setAttribute(Qt::WA_TranslucentBackground);

    setMouseTracking(true);
    ui->loadingWidget->setAttribute(Qt::WA_DeleteOnClose);
    ui->videoWidget->setMouseTracking(true);
    ui->videoWidget->hide();

    // 最后绘制，不设置最后绘制会影响父窗体异形异常（quickWidget的透明通道会形成穿透）
    ui->quickWidget->setAttribute(Qt::WA_AlwaysStackOnTop);
    // 背景透明
    ui->quickWidget->setClearColor(QColor(Qt::transparent));
}

void VideoForm::showToolFrom(bool show)
{
    if (!m_toolForm) {
        m_toolForm = new ToolForm(this, ToolForm::AP_OUTSIDE_RIGHT);
        m_toolForm->move(pos().x() + geometry().width(), pos().y() + 30);
    }
    m_toolForm->setVisible(show);
}

void VideoForm::updateStyleSheet(bool vertical)
{
    if (vertical) {
        setStyleSheet(R"(
                 #videoForm {
                     border-image: url(:/res/phone-v.png) 150px 142px 85px 142px;
                     border-width: 150px 142px 85px 142px;
                 }
                 )");
        layout()->setContentsMargins(10, 68, 12, 62);
    } else {
        setStyleSheet(R"(
                 #videoForm {
                     border-image: url(:/res/phone-h.png) 142px 85px 142px 150px;
                     border-width: 142px 85px 142px 150px;
                 }
                 )");
        layout()->setContentsMargins(68, 12, 62, 10);
    }
}

void VideoForm::updateShowSize(const QSize &newSize)
{
    if (frameSize != newSize) {
        frameSize = newSize;

        bool vertical = newSize.height() > newSize.width();
        QSize showSize = newSize;
        QDesktopWidget* desktop = QApplication::desktop();
        if (desktop) {
            QRect screenRect = desktop->availableGeometry();
            if (vertical) {
                showSize.setHeight(qMin(newSize.height(), screenRect.height() - 200));
                showSize.setWidth(showSize.height() * m_widthHeightRatio);
            } else {
                showSize.setWidth(qMin(newSize.width(), screenRect.width()));
                showSize.setHeight(showSize.width() * m_widthHeightRatio);
            }

            if (isFullScreen()) {
                switchFullScreen();
            }
            // 窗口居中
            move(screenRect.center() - QRect(0, 0, showSize.width(), showSize.height()).center());
        }

        // 减去标题栏高度 (mark:已经没有标题栏了)
        //int titleBarHeight = style()->pixelMetric(QStyle::PM_TitleBarHeight);
        //showSize.setHeight(showSize.height() - titleBarHeight);

        if (showSize != size()) {            
            resize(showSize);
            updateStyleSheet(vertical);
        }
    }
}

void VideoForm::switchFullScreen()
{
    if (isFullScreen()) {
        showNormal();
        updateStyleSheet(height() > width());
        showToolFrom(true);
    } else {
        showToolFrom(false);
        layout()->setContentsMargins(0, 0, 0, 0);
        showFullScreen();
    }
}

void VideoForm::postGoMenu()
{
    postKeyCodeClick(AKEYCODE_MENU);
}

void VideoForm::postGoBack()
{
    postKeyCodeClick(AKEYCODE_BACK);
}

void VideoForm::postAppSwitch()
{
    postKeyCodeClick(AKEYCODE_APP_SWITCH);
}

void VideoForm::postPower()
{
    postKeyCodeClick(AKEYCODE_POWER);
}

void VideoForm::postVolumeUp()
{
    postKeyCodeClick(AKEYCODE_VOLUME_UP);
}

void VideoForm::postVolumeDown()
{
    postKeyCodeClick(AKEYCODE_VOLUME_DOWN);
}

void VideoForm::postTurnOn()
{
    ControlEvent* controlEvent = new ControlEvent(ControlEvent::CET_COMMAND);
    if (!controlEvent) {
        return;
    }
    controlEvent->setCommandEventData(CONTROL_EVENT_COMMAND_BACK_OR_SCREEN_ON);
    m_inputConvert.sendControlEvent(controlEvent);
}

void VideoForm::postGoHome()
{
    postKeyCodeClick(AKEYCODE_HOME);
}

void VideoForm::postKeyCodeClick(AndroidKeycode keycode)
{
    ControlEvent* controlEventDown = new ControlEvent(ControlEvent::CET_KEYCODE);
    if (!controlEventDown) {
        return;
    }
    controlEventDown->setKeycodeEventData(AKEY_EVENT_ACTION_DOWN, keycode, AMETA_NONE);
    m_inputConvert.sendControlEvent(controlEventDown);

    ControlEvent* controlEventUp = new ControlEvent(ControlEvent::CET_KEYCODE);
    if (!controlEventUp) {
        return;
    }
    controlEventUp->setKeycodeEventData(AKEY_EVENT_ACTION_UP, keycode, AMETA_NONE);
    m_inputConvert.sendControlEvent(controlEventUp);
}

void VideoForm::mousePressEvent(QMouseEvent *event)
{
    if (ui->videoWidget->geometry().contains(event->pos())) {
        event->setLocalPos(ui->videoWidget->mapFrom(this, event->localPos().toPoint()));
        m_inputConvert.mouseEvent(event, ui->videoWidget->frameSize(), ui->videoWidget->size());
    } else {
        if (event->button() == Qt::LeftButton) {
            m_dragPosition = event->globalPos() - frameGeometry().topLeft();
            event->accept();
        }
    }
}

void VideoForm::mouseReleaseEvent(QMouseEvent *event)
{
    if (ui->videoWidget->geometry().contains(event->pos())) {
        event->setLocalPos(ui->videoWidget->mapFrom(this, event->localPos().toPoint()));
        m_inputConvert.mouseEvent(event, ui->videoWidget->frameSize(), ui->videoWidget->size());
    }
}

void VideoForm::mouseMoveEvent(QMouseEvent *event)
{    
    if (ui->videoWidget->geometry().contains(event->pos())) {
        event->setLocalPos(ui->videoWidget->mapFrom(this, event->localPos().toPoint()));
        m_inputConvert.mouseEvent(event, ui->videoWidget->frameSize(), ui->videoWidget->size());
    } else {
        if (event->buttons() & Qt::LeftButton) {
            move(event->globalPos() - m_dragPosition);
            event->accept();
        }
    }
}

void VideoForm::wheelEvent(QWheelEvent *event)
{
    if (ui->videoWidget->geometry().contains(event->pos())) {
        QPointF pos = ui->videoWidget->mapFrom(this, event->pos());
        /*
        QWheelEvent(const QPointF &pos, const QPointF& globalPos, int delta,
                Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers,
                Qt::Orientation orient = Qt::Vertical);
        */
        QWheelEvent wheelEvent(pos, event->globalPosF(), event->delta(),
                               event->buttons(), event->modifiers(), event->orientation());
        m_inputConvert.wheelEvent(&wheelEvent, ui->videoWidget->frameSize(), ui->videoWidget->size());
    }
}

void VideoForm::keyPressEvent(QKeyEvent *event)
{
    if (Qt::Key_Escape == event->key()
            && !event->isAutoRepeat()
            && isFullScreen()) {
        switchFullScreen();
    }
    //qDebug() << "keyPressEvent" << event->isAutoRepeat();
    m_inputConvert.keyEvent(event, ui->videoWidget->frameSize(), ui->videoWidget->size());
}

void VideoForm::keyReleaseEvent(QKeyEvent *event)
{
    //qDebug() << "keyReleaseEvent" << event->isAutoRepeat();
    m_inputConvert.keyEvent(event, ui->videoWidget->frameSize(), ui->videoWidget->size());
}

void VideoForm::paintEvent(QPaintEvent *paint)
{
    QStyleOption opt;
    opt.init(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void VideoForm::showEvent(QShowEvent *event)
{
    showToolFrom();
}