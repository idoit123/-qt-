#include "tcpclient.h"
#include "ui_tcpclient.h"
#include"protocol.h"
#include <QNetworkProxy>
TcpClient::TcpClient(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::TcpClient)
{

    ui->setupUi(this);
    this->loadConfig();
    connect(&m_tcpSocket, SIGNAL(connected()), this,SLOT(showConnect()));//信号的发送者、发送的是什么信号、信号的处理者，怎么处理
    connect(&m_tcpSocket, SIGNAL(readyRead()), this,SLOT(recvMsg())); //只要客户端收到消息，就会产生readyRead()信号
    connect(&m_tcpSocket, &QTcpSocket::disconnected, this, [this]() {
        m_framer.clear();
    });
    connect(&m_tcpSocket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        qWarning() << "Server connection error:" << m_tcpSocket.errorString();
    });
     this->m_tcpSocket.connectToHost(QHostAddress(m_strIp), m_usPort);
}

TcpClient::~TcpClient()
{
    delete ui;
}
TcpClient &TcpClient::getInstance()
{
    static TcpClient instance;
    return instance;
}

QTcpSocket &TcpClient::getTcpSocket()
{
    return m_tcpSocket;
}
void TcpClient::loadConfig()
{
    QFile file(":/client.config");
    if (file.open(QIODevice::ReadOnly)) {
        //只读方式打开
        QByteArray baData = file.readAll();//以字节方式读取
        QString strData = baData.toStdString().c_str();
        qDebug() << strData;
        file.close();
        strData.replace("\r\n", " "); //\r\n回车换行
        qDebug() << strData;

        QStringList strlist = strData.split(" ");
        for (int i = 0; i < strlist.size(); i++) {
            qDebug() << "--->" << strlist[i];
        }

        this->m_strIp = strlist[0];
        this->m_usPort = strlist[1].toUShort();
        const QString boardIp = qEnvironmentVariable("CHAT_SERVER_IP");
        QHostAddress boardAddress;
        if (!boardIp.isEmpty() && boardAddress.setAddress(boardIp)) {
            this->m_strIp = boardIp;
            m_tcpSocket.setProxy(QNetworkProxy::NoProxy);
        }
        qDebug() << "ip:" << this->m_strIp;
        qDebug() << "port:" << this->m_usPort;
    }
    else {
        QMessageBox::critical(this, "open config", "open config falsed");
    }
}

void TcpClient::showConnect()
{
    qInfo() << "Connected to chat server:" << m_tcpSocket.peerAddress().toString()
            << m_tcpSocket.peerPort();
    QMessageBox::information(this,"连接服务器","连接服务器成功");
}

void TcpClient::recvMsg()
{
    m_framer.append(m_tcpSocket.readAll());
    while (true) {
        QByteArray frame;
        QString error;
        const PduFramer::Result result = m_framer.takeFrame(&frame, &error);
        if (result == PduFramer::NeedMoreData) return;
        if (result == PduFramer::InvalidFrame) {
            qWarning() << "Invalid server response:" << error;
            m_framer.clear();
            m_tcpSocket.abort();
            return;
        }

        PDU *pdu = static_cast<PDU *>(malloc(static_cast<size_t>(frame.size())));
        if (!pdu) {
            qCritical() << "Cannot allocate memory for server response";
            m_framer.clear();
            m_tcpSocket.abort();
            return;
        }
        memcpy(pdu, frame.constData(), static_cast<size_t>(frame.size()));
        processPdu(pdu);
        free(pdu);
    }
}

void TcpClient::processPdu(PDU *pdu)
{
     switch(pdu->uiMsgType){
         //客户端接收到服务器端发送的注册回复信号
     case ENUM_MSG_TYPE_REGIST_RESPOND:
     {
         if(0 == strcmp(pdu->caData, REGIST_OK)){
             //strcmp比较字符串
             QMessageBox::information(this,"注册",REGIST_OK);
         }
         else{
             QMessageBox::warning(this,"注册",REGIST_FAILED);
         }
         break;
     }
     case ENUM_MSG_TYPE_LOGIN_RESPOND:
     {
         if(0 == strcmp(pdu->caData, LOGIN_OK)){
             //strcmp比较字符串
             //登录成功之后，更新当前路径
             m_strCurPath = QString("./%1").arg(m_strLoginName);
             qDebug() << m_strCurPath;
             QMessageBox::information(this,"登录",LOGIN_OK);
             OpeWidget::getInstance().show();
             OpeWidget::getInstance().getBook()->reset();
             OpeWidget::getInstance().getBook()->flushDir();
             this->hide();
         }
         else{
             QMessageBox::warning(this,"登录",LOGIN_FAILED);
         }
         break;
     }
     case ENUM_MSG_TYPE_ALL_ONLINE_RESPOND:
        {
            OpeWidget::getInstance().getFriend()->showALLOnlineUsr(pdu);
            break;
            //这里目前存在bug，也就是会重复显示，lineEdit中保留了上一次的结果
        }
     case ENUM_MSG_TYPE_SEARCH_RESPOND:
        {
            //用户不存在
            if(strcmp(SEARCH_USR_NO,pdu->caData) == 0){
                QMessageBox::information(this,"搜索",QString("\'%1\' no exist!").arg(OpeWidget::getInstance().getFriend()->m_strSearchName));
            }
            //用户在线
            else if(strcmp(SEARCH_USR_ONLINE,pdu->caData) == 0){
                QMessageBox::information(this,"搜索",QString("\'%1\' online!").arg(OpeWidget::getInstance().getFriend()->m_strSearchName));
            }
            //用户不在线
            else if(strcmp(SEARCH_USR_OFFLINE,pdu->caData) == 0){
                QMessageBox::information(this,"搜索",QString("\'%1\' offline!").arg(OpeWidget::getInstance().getFriend()->m_strSearchName));
            }
            break;
        }
         //客户端接收到服务器端发送的添加好友响应
        case ENUM_MSG_TYPE_ADD_FRIEND_RESPOND:
        {
            //未知错误
            if(strcmp(UNKNOW_ERROR,pdu->caData) == 0){
                QMessageBox::critical(this,"添加好友",UNKNOW_ERROR);
            }
            //已经是好友了
            else if(strcmp(EXISTED_FRIEND,pdu->caData) == 0){
                QMessageBox::information(this,"添加好友",EXISTED_FRIEND);
            }
            //不是好友但是对方已经离线
            else if(strcmp(ADD_FRIEND_OFFLINE,pdu->caData) == 0){
                QMessageBox::information(this,"添加好友",ADD_FRIEND_OFFLINE);
            }
            //用户不存在
            else if(strcmp(ADD_FRIEND_NOEXIST,pdu->caData) == 0){
                qDebug() << "ADD_FRIEND_NOEXIST";
                QMessageBox::information(this,"添加好友",ADD_FRIEND_NOEXIST);
            }
            break;
        }
     case ENUM_MSG_TYPE_ADD_FRIEND_REQUEST:
        {
            char requestName[32] = {'\0'}; //A的姓名
            char friName[32] = {'\0'}; //B的姓名
            strncpy(requestName,pdu->caData,32);
            strncpy(friName,pdu->caData+32,32);
            qDebug() << "strncpy(requestName,pdu->caData,32)";
            int ret = QMessageBox::information(this,"添加好友",QString("%1 want to add you as friend, do you agree?").arg(requestName),
                                                QMessageBox::Yes, QMessageBox::No);
            PDU *respdu = mkPDU(0);
            if(ret == QMessageBox::Yes){
                //同意添加
                respdu->uiMsgType = ENUM_MSG_TYPE_ADD_FRIEND_AGREE;
            }
            else{
                //拒绝添加
                respdu->uiMsgType = ENUM_MSG_TYPE_ADD_FRIEND_REFUSE;
            }
            //数据处理后发送给服务器
            memcpy(respdu->caData,requestName,32);
            memcpy(respdu->caData+32,friName,32);
            m_tcpSocket.write((char*)respdu,respdu->uiPDULen);
            free(respdu);
            respdu = NULL;
            break;
        }
            //收到同意添加为好友回复
        case ENUM_MSG_TYPE_ADD_FRIEND_AGREE:
        {
            char friName[32] = {'\0'}; //B的姓名
            strncpy(friName,pdu->caData,32);
            QMessageBox::information(this,"添加好友",QString("%1 agree to add you as friend!").arg(friName));
            break;
        }
            //收到拒绝添加为好友的回复
        case ENUM_MSG_TYPE_ADD_FRIEND_REFUSE:
        {
            char friName[32] = {'\0'}; //B的姓名
            strncpy(friName,pdu->caData,32);
            QMessageBox::information(this,"添加好友",QString("%1 refuse to add you as friend!").arg(friName));
            break;
        }
         //收到服务器的刷新好友列表回复
       case ENUM_MSG_TYPE_FLUSH_FRIEND_RESPOND:
       {
           OpeWidget::getInstance().getFriend()->showFriendName(pdu);
           break;
       }
         //收到服务器端的删除好友成功回复
         case ENUM_MSG_TYPE_DELETE_FRIEND_RESPOND_TRUE:
         {
             char usrName[32] = {'\0'};
             char friName[32] = {'\0'};
             //从pdu中获取用户姓名以及要删除好友的姓名
             strncpy(usrName,pdu->caData,32);
             strncpy(friName,pdu->caData+32,32);
             QMessageBox::information(this,"删除好友",QString("\'%1\' is successfully deleted").arg(friName));
             break;
         }
             //收到服务器的删除好友失败回复
         case ENUM_MSG_TYPE_DELETE_FRIEND_RESPOND_FAILED:
         {
             char usrName[32] = {'\0'};
             char friName[32] = {'\0'};
             //从pdu中获取用户姓名以及要删除好友的姓名
             strncpy(usrName,pdu->caData,32);
             strncpy(friName,pdu->caData+32,32);
             QMessageBox::warning(this,"删除好友",QString("fail to delete, \'%1\' may not be your friend").arg(friName));
             break;
         }
         //客户端收到转发的私聊请求
          case ENUM_MSG_TYPE_PRIVATE_CHAT_REQUEST:
          {
              if(PrivateChat::getInstance().isHidden()){
                  PrivateChat::getInstance().show();
              }
              char sendName[32] = {'\0'};//信息的发送者
              char receiveName[32] = {'\0'};//信息的接收者
              strncpy(sendName,pdu->caData,32);
              strncpy(receiveName,pdu->caData+32,32);
              QString msg = (char*)pdu->caMsg;
              //在弹出的窗口中显示信息
              PrivateChat::getInstance().setChatName(sendName);
              PrivateChat::getInstance().updateMsg(pdu);
              break;
          }
     case ENUM_MSG_TYPE_GROUP_CHAT_RESPOND:
         {
             OpeWidget::getInstance().getFriend()->showGroupMsg(pdu);
             break;
         }
     case ENUM_MSG_TYPE_FILE_RESPOND:
         OpeWidget::getInstance().getBook()->handleResponse(pdu);
         break;
     default:
         break;
     }
}


//void TcpClient::on_send_pb_clicked()
//{
//    QString strMsg=ui->lineEdit->text();
//    if(!strMsg.isEmpty()){
//        PDU *pdu=mkPDU(strMsg.size());
//        memcpy(pdu->caMsg,strMsg.toStdString().c_str(),strMsg.size());
//        m_tcpSocket.write((char *)pdu,pdu->uiPDULen);
//        free(pdu);
//        pdu=NULL;
//    }
//    else{
//        QMessageBox::warning(this,"empty fault","empyt fault");
//    }
//}

void TcpClient::on_login_pb_clicked()
{
    QString strName = ui->name_le->text();
    QString strPwd = ui->pwd_le->text();

    if(!strName.isEmpty() && !strPwd.isEmpty()){
        this->m_strLoginName = strName;
        PDU *pdu = mkPDU(0);
        pdu->uiMsgType = ENUM_MSG_TYPE_LOGIN_REQUEST;//将消息类型设置为登录请求
        //pdu->caData的前32位放用户名，后32位放密码
        strncpy(pdu->caData,strName.toStdString().c_str(),32);
        strncpy(pdu->caData+32,strPwd.toStdString().c_str(),32);
        m_tcpSocket.write((char*)pdu,pdu->uiPDULen); //将消息发送给服务器端
        free(pdu); //发送完消息后释放空间
        pdu = NULL;
    }
    else{
        QMessageBox::critical(this,"登录","login error！");
    }
}

void TcpClient::on_regist_pb_clicked()
{
    QString strName = ui->name_le->text();
     QString strPwd = ui->pwd_le->text();
     if(!strName.isEmpty() || !strPwd.isEmpty()){
             PDU *pdu = mkPDU(0);
             pdu->uiMsgType = ENUM_MSG_TYPE_REGIST_REQUEST;//将消息类型设置为注册请求
             //pdu->caData的前32位放用户名，后32位放密码
             strncpy(pdu->caData,strName.toStdString().c_str(),32);
             strncpy(pdu->caData+32,strPwd.toStdString().c_str(),32);
             m_tcpSocket.write((char*)pdu,pdu->uiPDULen); //将消息发送给服务器端
             free(pdu); //发送完消息后释放空间
             pdu = NULL;
         }
         else{
             QMessageBox::critical(this,"注册","注册失败：用户名或密码为空！");
         }

}
QString TcpClient::getLoginName()
{
    return this->m_strLoginName;
}

QString TcpClient::curPath()
{
    return m_strCurPath;
}

void TcpClient::updateCurPath(QString str_curPath)
{
    m_strCurPath = str_curPath;
}

void TcpClient::on_cancel_pb_clicked()
{

}
