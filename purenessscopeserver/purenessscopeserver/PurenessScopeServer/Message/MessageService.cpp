// MessageService.h
// 处理消息的线程类，Connect会将要处理的CMessage对象放入这些线程中处理。
// 以为有些故事，以为过久了就会忘掉。却发现，沉淀的却是自我，慢慢的，故事变成了信仰。
// add by freeeyes
// 2009-01-26

#include "MessageService.h"

CMessageService::CMessageService():m_mutex(), m_cond(m_mutex)
{
    m_u4ThreadID         = 0;
    m_u4MaxQueue         = MAX_MSG_THREADQUEUE;
    m_blRun              = false;
    m_u4HighMask         = 0;
    m_u4LowMask          = 0;
    m_u8TimeCost         = 0;
    m_u4Count            = 0;
    m_emThreadState      = THREAD_STOP;
    m_u2ThreadTimeCheck  = 0;
    m_u4WorkQueuePutTime = 0;

    uint16 u2ThreadTimeOut = App_MainConfig::instance()->GetThreadTimuOut();

    if(u2ThreadTimeOut == 0)
    {
        m_u2ThreadTimeOut = MAX_MSG_THREADTIMEOUT;
    }
    else
    {
        m_u2ThreadTimeOut = u2ThreadTimeOut;
    }
}

CMessageService::~CMessageService()
{
    OUR_DEBUG((LM_INFO, "[CMessageService::~CMessageService].\n"));
}

void CMessageService::Init(uint32 u4ThreadID, uint32 u4MaxQueue, uint32 u4LowMask, uint32 u4HighMask)
{
    m_u4MaxQueue    = u4MaxQueue;
    m_u4HighMask    = u4HighMask;
    m_u4LowMask     = u4LowMask;

    //OUR_DEBUG((LM_INFO, "[CMessageService::Init]ID=%d,m_u4State=%d.\n", m_u4ThreadID = u4ThreadID, m_ThreadInfo.m_u4State));

    //添加线程信息
    m_u4ThreadID = u4ThreadID;
    m_ThreadInfo.m_u4ThreadID   = u4ThreadID;

    m_u4WorkQueuePutTime = App_MainConfig::instance()->GetWorkQueuePutTime() * 1000;

    //初始化线程AI
    m_WorkThreadAI.Init(App_MainConfig::instance()->GetWTAI(),
                        App_MainConfig::instance()->GetPacketTimeOut(),
                        App_MainConfig::instance()->GetWTCheckTime(),
                        App_MainConfig::instance()->GetWTTimeoutCount(),
                        App_MainConfig::instance()->GetWTStopTime(),
                        App_MainConfig::instance()->GetWTReturnDataType(),
                        App_MainConfig::instance()->GetWTReturnData());

    //按照线程初始化统计模块的名字
    char szName[MAX_BUFF_50] = {'\0'};
    sprintf_safe(szName, MAX_BUFF_50, "工作线程(%d)", u4ThreadID);
    m_CommandAccount.InitName(szName, App_MainConfig::instance()->GetMaxCommandCount());

    //初始化统计模块功能
    m_CommandAccount.Init(App_MainConfig::instance()->GetCommandAccount(),
                          App_MainConfig::instance()->GetCommandFlow(),
                          App_MainConfig::instance()->GetPacketTimeOut());

    //初始化本地信令列表副本
    m_objClientCommandList.Init(App_MessageManager::instance()->GetMaxCommandCount());

    //初始化CommandID告警阀值相关
    for(int i = 0; i < (int)App_MainConfig::instance()->GetCommandAlertCount(); i++)
    {
        _CommandAlert* pCommandAlert = App_MainConfig::instance()->GetCommandAlert(i);

        if(NULL != pCommandAlert)
        {
            m_CommandAccount.AddCommandAlert(pCommandAlert->m_u2CommandID,
                                             pCommandAlert->m_u4CommandCount,
                                             pCommandAlert->m_u4MailID);
        }
    }

    //初始化工作线程历史记录
    if (true == App_MainConfig::instance()->GetWorkThreadChart()->m_blJsonOutput)
    {
        m_objThreadHistoryList.Init(App_MainConfig::instance()->GetWorkThreadChart()->m_u2Count);
    }

    //设置消息池
    m_MessagePool.Init(MAX_MESSAGE_POOL, CMessagePool::Init_Callback);
}

bool CMessageService::Start()
{
    m_emThreadState = THREAD_RUN;

    if(0 != open())
    {
        m_emThreadState = THREAD_STOP;
        return false;
    }

    return true;
}

int CMessageService::open(void* args)
{
    if(args != NULL)
    {
        OUR_DEBUG((LM_INFO,"[CMessageService::open]args is not NULL.\n"));
    }

    m_blRun = true;
    msg_queue()->high_water_mark(m_u4HighMask);
    msg_queue()->low_water_mark(m_u4LowMask);

    OUR_DEBUG((LM_INFO,"[CMessageService::open] m_u4HighMask = [%d] m_u4LowMask = [%d]\n", m_u4HighMask, m_u4LowMask));

    if(activate(THREAD_PARAM, MAX_MSG_THREADCOUNT) == -1)
    {
        OUR_DEBUG((LM_ERROR, "[CMessageService::open] activate error ThreadCount = [%d].\n", MAX_MSG_THREADCOUNT));
        m_blRun = false;
        return -1;
    }

    resume();
    return 0;
}

int CMessageService::svc(void)
{
    // Cache our ACE_Thread_Manager pointer.
    ACE_Thread_Manager* mgr = this->thr_mgr ();

    while(true)
    {
        if (mgr->testcancel(mgr->thr_self ()))
        {
            return 0;
        }

        ACE_Message_Block* mb = NULL;
        ACE_OS::last_error(0);

        //xtime = ACE_OS::gettimeofday() + ACE_Time_Value(0, MAX_MSG_PUTTIMEOUT);
        if(getq(mb, 0) == -1)
        {
            OUR_DEBUG((LM_ERROR,"[CMessageService::svc] PutMessage error errno = [%d].\n", ACE_OS::last_error()));
            m_blRun = false;
            break;
        }
        else
        {
            if(mb == NULL)
            {
                continue;
            }

            if ((mb->msg_type() == ACE_Message_Block::MB_USER))
            {
                uint32 u4UpdateIndex = 0;
                memcpy_safe(mb->rd_ptr(), sizeof(int), (char* )&u4UpdateIndex, sizeof(int));
                OUR_DEBUG((LM_ERROR, "[CMessageService::svc](%d)<UpDateIndex=%d>CopyMessageManagerList.\n", m_ThreadInfo.m_u4ThreadID, u4UpdateIndex));

                if (u4UpdateIndex > 0)
                {
                    int nReload = App_ModuleLoader::instance()->UnloadListUpdate(u4UpdateIndex);

                    if (1 == nReload)
                    {
                        //需要通知大家再更新一下副本(让新的加载生效)
                        App_MessageServiceGroup::instance()->PutUpdateCommandMessage(App_MessageManager::instance()->GetUpdateIndex());
                    }

                    //同步信令列表
                    CopyMessageManagerList();
                }

                App_MessageBlockManager::instance()->Close(mb);
                continue;
            }

            if ((0 == mb->size ()) && (mb->msg_type () == ACE_Message_Block::MB_STOP))
            {
                m_mutex.acquire();
                mb->release ();
                this->msg_queue ()->deactivate ();
                m_cond.signal();
                m_mutex.release();
                break;
            }

            while(m_emThreadState != THREAD_RUN)
            {
                //如果模块正在卸载或者重载，线程在这里等加载完毕（等1ms）。
                ACE_Time_Value tvsleep(0, 1000);
                ACE_OS::sleep(tvsleep);
            }

            CMessage* msg = *((CMessage**)mb->base());

            if(!msg)
            {
                OUR_DEBUG((LM_ERROR,"[CMessageService::svc] mb msg == NULL CurrthreadNo=[%d]!\n", m_u4ThreadID));
                continue;
            }

            if (false == this->ProcessMessage(msg, m_u4ThreadID))
            {
                OUR_DEBUG((LM_ERROR, "[CMessageService::svc](%d)ProcessMessage is false!\n", m_u4ThreadID));
            }
        }

        //使用内存池，这块内存不必再释放
    }

    OUR_DEBUG((LM_INFO,"[CMessageService::svc] svc finish!\n"));
    return 0;
}

bool CMessageService::PutMessage(CMessage* pMessage)
{
    ACE_Message_Block* mb = pMessage->GetQueueMessage();

    if(NULL != mb)
    {
        //判断队列是否是已经最大
        int nQueueCount = (int)msg_queue()->message_count();

        if(nQueueCount >= (int)m_u4MaxQueue)
        {
            OUR_DEBUG((LM_ERROR,"[CMessageService::PutMessage] Queue is Full nQueueCount = [%d].\n", nQueueCount));
            return false;
        }

        ACE_Time_Value xtime = ACE_OS::gettimeofday() + ACE_Time_Value(0, m_u4WorkQueuePutTime);

        if(this->putq(mb, &xtime) == -1)
        {
            OUR_DEBUG((LM_ERROR,"[CMessageService::PutMessage] Queue putq  error nQueueCount = [%d] errno = [%d].\n", nQueueCount, errno));
            return false;
        }
    }
    else
    {
        OUR_DEBUG((LM_ERROR,"[CMessageService::PutMessage] mb new error.\n"));
        return false;
    }

    return true;
}

bool CMessageService::PutUpdateCommandMessage(uint32 u4UpdateIndex)
{
    ACE_Message_Block* mblk = App_MessageBlockManager::instance()->Create(sizeof(int));

    if (NULL == mblk)
    {
        return false;
    }

    memcpy_safe((char* )&u4UpdateIndex, sizeof(int), mblk->wr_ptr(), sizeof(int));
    mblk->wr_ptr(sizeof(int));

    mblk->msg_type(ACE_Message_Block::MB_USER);

    //判断队列是否是已经最大
    int nQueueCount = (int)msg_queue()->message_count();

    if (nQueueCount >= (int)m_u4MaxQueue)
    {
        OUR_DEBUG((LM_ERROR, "[CMessageService::PutUpdateCommandMessage] Queue is Full nQueueCount = [%d].\n", nQueueCount));
        return false;
    }

    ACE_Time_Value xtime = ACE_OS::gettimeofday() + ACE_Time_Value(0, m_u4WorkQueuePutTime);

    if (this->putq(mblk, &xtime) == -1)
    {
        OUR_DEBUG((LM_ERROR, "[CMessageService::PutUpdateCommandMessage] Queue putq  error nQueueCount = [%d] errno = [%d].\n", nQueueCount, errno));
        return false;
    }

    return true;
}

bool CMessageService::ProcessMessage(CMessage* pMessage, uint32 u4ThreadID)
{
    //CProfileTime DisposeTime;
    //uint32 u4Cost = (uint32)(pMessage->GetMessageBase()->m_ProfileTime.Stop());

    if(NULL == pMessage)
    {
        OUR_DEBUG((LM_ERROR,"[CMessageService::ProcessMessage] [%d]pMessage is NULL.\n", u4ThreadID));
        return false;
    }

    if(NULL == pMessage->GetMessageBase())
    {
        OUR_DEBUG((LM_ERROR,"[CMessageService::ProcessMessage] [%d]pMessage->GetMessageBase() is NULL.\n", u4ThreadID));
        DeleteMessage(pMessage);
        return false;
    }

    //在这里进行线程自检代码
    m_ThreadInfo.m_tvUpdateTime = ACE_OS::gettimeofday();
    m_ThreadInfo.m_u4State = THREAD_RUNBEGIN;

    //判断队列处理时间是否超过了数据入队列的时间
    ACE_Time_Value tvQueueDispose(m_ThreadInfo.m_tvUpdateTime - pMessage->GetMessageBase()->m_tvRecvTime);

    if (tvQueueDispose.msec() > (uint32)App_MainConfig::instance()->GetPacketTimeOut())
    {
        AppLogManager::instance()->WriteLog(LOG_SYSTEM_COMMANDDATA, "[CMessageService::ProcessMessage]CommandID=0x%04x, Queue put dispose time interval(%d).\n",
                                            (int)pMessage->GetMessageBase()->m_u2Cmd,
                                            tvQueueDispose.msec());
    }

    //OUR_DEBUG((LM_ERROR,"[CMessageService::ProcessMessage]1 [%d],m_u4State=%d, commandID=%d.\n", u4ThreadID, m_ThreadInfo.m_u4State,  pMessage->GetMessageBase()->m_u2Cmd));

    //将要处理的数据放到逻辑处理的地方去
    uint16 u2CommandID = 0;          //数据包的CommandID

    u2CommandID = pMessage->GetMessageBase()->m_u2Cmd;

    //抛出掉链接建立和断开，只计算逻辑数据包
    if(pMessage->GetMessageBase()->m_u2Cmd != CLIENT_LINK_CONNECT
       && pMessage->GetMessageBase()->m_u2Cmd != CLIENT_LINK_CDISCONNET
       && pMessage->GetMessageBase()->m_u2Cmd != CLIENT_LINK_SDISCONNET
       && pMessage->GetMessageBase()->m_u2Cmd != CLINET_LINK_SENDTIMEOUT
       && pMessage->GetMessageBase()->m_u2Cmd != CLINET_LINK_SENDERROR
       && pMessage->GetMessageBase()->m_u2Cmd != CLINET_LINK_CHECKTIMEOUT
       &&  pMessage->GetMessageBase()->m_u2Cmd != CLIENT_LINK_SENDOK)
    {
        m_ThreadInfo.m_u4RecvPacketCount++;
        m_ThreadInfo.m_u4CurrPacketCount++;
        m_ThreadInfo.m_u2CommandID   = u2CommandID;

        bool blIsDead = m_WorkThreadAI.CheckCurrTimeout(pMessage->GetMessageBase()->m_u2Cmd, (uint32)m_ThreadInfo.m_tvUpdateTime.sec());

        if(blIsDead == true)
        {
            OUR_DEBUG((LM_ERROR,"[CMessageService::ProcessMessage]Command(%d) is Delele.\n", pMessage->GetMessageBase()->m_u2Cmd));
            //直接返回应急数据给客户端，不在到逻辑里去处理

            const char* ptrReturnData = reinterpret_cast<const char*>(m_WorkThreadAI.GetReturnData());
#ifdef WIN32
            App_ProConnectManager::instance()->PostMessage(pMessage->GetMessageBase()->m_u4ConnectID,
                    ptrReturnData,
                    m_WorkThreadAI.GetReturnDataLength(),
                    SENDMESSAGE_NOMAL,
                    (uint16)COMMAND_RETURN_BUSY,
                    PACKET_SEND_IMMEDIATLY,
                    PACKET_IS_SELF_RECYC);
#else
            App_ConnectManager::instance()->PostMessage(pMessage->GetMessageBase()->m_u4ConnectID,
                    ptrReturnData,
                    m_WorkThreadAI.GetReturnDataLength(),
                    SENDMESSAGE_NOMAL,
                    (uint16)COMMAND_RETURN_BUSY,
                    PACKET_SEND_IMMEDIATLY,
                    PACKET_IS_SELF_RECYC);
#endif
            DeleteMessage(pMessage);
            m_ThreadInfo.m_u4State = THREAD_RUNEND;

            return true;
        }
    }

    uint32 u4TimeCost     = 0;      //命令执行时间
    uint16 u2CommandCount = 0;      //命令被调用次数
    bool   blDeleteFlag   = true;   //用完是否删除，默认是删除

    DoMessage(m_ThreadInfo.m_tvUpdateTime, pMessage, u2CommandID, u4TimeCost, u2CommandCount, blDeleteFlag);

    if(pMessage->GetMessageBase()->m_u2Cmd != CLIENT_LINK_CONNECT
       && pMessage->GetMessageBase()->m_u2Cmd != CLIENT_LINK_CDISCONNET
       && pMessage->GetMessageBase()->m_u2Cmd != CLIENT_LINK_SDISCONNET
       && pMessage->GetMessageBase()->m_u2Cmd != CLINET_LINK_SENDTIMEOUT
       && pMessage->GetMessageBase()->m_u2Cmd != CLINET_LINK_SENDERROR
       && pMessage->GetMessageBase()->m_u2Cmd != CLINET_LINK_CHECKTIMEOUT
       &&  pMessage->GetMessageBase()->m_u2Cmd != CLIENT_LINK_SENDOK)
    {
        //如果AI启动了，则在这里进行AI判定
        m_WorkThreadAI.SaveTimeout(pMessage->GetMessageBase()->m_u2Cmd, u4TimeCost);

        if(u2CommandCount > 0)
        {
            //获得单个命令的执行时间
            u4TimeCost = u4TimeCost/u2CommandCount;
        }

        //OUR_DEBUG((LM_ERROR,"[CMessageService::ProcessMessage]Command(%d)=[%d].\n", pMessage->GetMessageBase()->m_u2Cmd, u2CommandCount));

        //添加统计信息
        m_CommandAccount.SaveCommandData(u2CommandID,
                                         pMessage->GetMessageBase()->m_u4ListenPort,
                                         pMessage->GetMessageBase()->m_u1PacketType,
                                         pMessage->GetMessageBase()->m_u4HeadSrcSize + pMessage->GetMessageBase()->m_u4BodySrcSize,
                                         COMMAND_TYPE_IN);
    }

    if (true == blDeleteFlag)
    {
        DeleteMessage(pMessage);
    }

    m_ThreadInfo.m_u4State = THREAD_RUNEND;

    //开始测算数据包处理的时间
    if(m_ThreadInfo.m_u2PacketTime == 0)
    {
        m_ThreadInfo.m_u2PacketTime = (uint16)u4TimeCost;
    }
    else
    {
        //计算数据包的平均处理时间
        m_ThreadInfo.m_u2PacketTime = (uint16)((m_ThreadInfo.m_u2PacketTime + (uint16)u4TimeCost)/2);
    }

    return true;
}

int CMessageService::Close()
{
    if(m_blRun)
    {
        m_blRun = false;

        if (false == this->CloseMsgQueue())
        {
            OUR_DEBUG((LM_INFO, "[CMessageService::Close]CloseMsgQueue is fail.\n"));
        }
    }
    else
    {
        m_blRun = false;
        msg_queue()->deactivate();
    }

    m_MessagePool.Close();

    CloseCommandList();

    m_objClientCommandList.Close();

    OUR_DEBUG((LM_INFO, "[CMessageService::close] Close().\n"));
    return 0;
}

bool CMessageService::SaveThreadInfoData()
{
    //这里进行线程自检
    ACE_Time_Value tvNow(ACE_OS::gettimeofday());
    ACE_Date_Time dt(m_ThreadInfo.m_tvUpdateTime);

    //添加到线程信息历史数据表
    _ThreadInfo objCurrThreadInfo    = m_ThreadInfo;
    objCurrThreadInfo.m_tvUpdateTime = ACE_OS::gettimeofday();
    m_objThreadHistoryList.AddObject(objCurrThreadInfo);

    //开始查看线程是否超时
    //OUR_DEBUG((LM_INFO, "[CMessageService::SaveThreadInfoData]ID=%d,m_u4State=%d,m_u2ThreadTimeOut=%d,cost=%d.\n", m_ThreadInfo.m_u4ThreadID, m_ThreadInfo.m_u4State, m_u2ThreadTimeOut, tvNow.sec() - m_ThreadInfo.m_tvUpdateTime.sec()));
    if(m_ThreadInfo.m_u4State == THREAD_RUNBEGIN && tvNow.sec() - m_ThreadInfo.m_tvUpdateTime.sec() > m_u2ThreadTimeOut)
    {
        AppLogManager::instance()->WriteLog(LOG_SYSTEM_WORKTHREAD, "[CMessageService::handle_timeout] pThreadInfo = [%d] State = [%d] Time = [%04d-%02d-%02d %02d:%02d:%02d] PacketCount = [%d] LastCommand = [0x%x] PacketTime = [%d] TimeOut > %d[%d] CurrPacketCount = [%d] QueueCount = [%d] BuffPacketUsed = [%d] BuffPacketFree = [%d].",
                                            m_ThreadInfo.m_u4ThreadID,
                                            m_ThreadInfo.m_u4State,
                                            dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second(),
                                            m_ThreadInfo.m_u4RecvPacketCount,
                                            m_ThreadInfo.m_u2CommandID,
                                            m_ThreadInfo.m_u2PacketTime,
                                            m_u2ThreadTimeOut,
                                            tvNow.sec() - m_ThreadInfo.m_tvUpdateTime.sec(),
                                            m_ThreadInfo.m_u4CurrPacketCount,
                                            (int)msg_queue()->message_count(),
                                            App_BuffPacketManager::instance()->GetBuffPacketUsedCount(),
                                            App_BuffPacketManager::instance()->GetBuffPacketFreeCount());

        //发现阻塞线程，需要重启相应的线程
        AppLogManager::instance()->WriteLog(LOG_SYSTEM_WORKTHREAD, "[CMessageService::handle_timeout] ThreadID = [%d] Thread is reset.", m_u4ThreadID);
        return false;
    }
    else
    {
        AppLogManager::instance()->WriteLog(LOG_SYSTEM_WORKTHREAD, "[CMessageService::handle_timeout] pThreadInfo = [%d] State = [%d] Time = [%04d-%02d-%02d %02d:%02d:%02d] PacketCount = [%d] LastCommand = [0x%x] PacketTime = [%d] CurrPacketCount = [%d] QueueCount = [%d] BuffPacketUsed = [%d] BuffPacketFree = [%d].",
                                            m_ThreadInfo.m_u4ThreadID,
                                            m_ThreadInfo.m_u4State,
                                            dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second(),
                                            m_ThreadInfo.m_u4RecvPacketCount,
                                            m_ThreadInfo.m_u2CommandID,
                                            m_ThreadInfo.m_u2PacketTime,
                                            m_ThreadInfo.m_u4CurrPacketCount,
                                            (int)msg_queue()->message_count(),
                                            App_BuffPacketManager::instance()->GetBuffPacketUsedCount(),
                                            App_BuffPacketManager::instance()->GetBuffPacketFreeCount());

        m_ThreadInfo.m_u4CurrPacketCount = 0;
        return true;
    }
}

bool CMessageService::GetThreadInfoJson(char* pJson, uint32 u4Len)
{
    //将数据输出成Json格式数据
    char szDataInfo[MAX_BUFF_200] = { '\0' };
    vector<_ThreadInfo> objVecHistoryList;
    m_objThreadHistoryList.GetAllSavingObject(objVecHistoryList);

    for(int i = 0; i < (int)objVecHistoryList.size(); i++)
    {
        if (0 == i)
        {
            sprintf_safe(szDataInfo, MAX_BUFF_200, "%d", objVecHistoryList[i].m_u4CurrPacketCount);
        }
        else
        {
            char szTemp[MAX_BUFF_500] = { '\0' };
            sprintf_safe(szTemp, MAX_BUFF_500, "%s", szDataInfo);
            sprintf_safe(szDataInfo, MAX_BUFF_200, "%s,%d", szTemp, objVecHistoryList[i].m_u4CurrPacketCount);
        }
    }

    sprintf_safe(pJson, u4Len, OUTPUT_CHART_JSON_Y, m_u4ThreadID, szDataInfo);

    return true;
}

bool CMessageService::GetThreadInfoTimeJson(char* pJson, uint32 u4Len)
{
    //将数据输出成Json格式数据
    char szDataInfo[MAX_BUFF_500] = { '\0' };
    vector<_ThreadInfo> objVecHistoryList;
    m_objThreadHistoryList.GetAllSavingObject(objVecHistoryList);

    for (int i = 0; i < (int)objVecHistoryList.size(); i++)
    {
        ACE_Date_Time dtThreadTime(objVecHistoryList[i].m_tvUpdateTime);

        if (0 == i)
        {
            sprintf_safe(szDataInfo, MAX_BUFF_500, "\"%02d:%02d:%02d\"",
                         dtThreadTime.hour(),
                         dtThreadTime.minute(),
                         dtThreadTime.second());
        }
        else
        {
            char szTemp[MAX_BUFF_500] = { '\0' };
            sprintf_safe(szTemp, MAX_BUFF_500, "%s", szDataInfo);
            sprintf_safe(szDataInfo, MAX_BUFF_500, "%s,\"%02d:%02d:%02d\"",
                         szTemp,
                         dtThreadTime.hour(),
                         dtThreadTime.minute(),
                         dtThreadTime.second());
        }
    }

    sprintf_safe(pJson, u4Len, OUTPUT_CHART_JSON_X, szDataInfo);

    return true;
}

void CMessageService::CloseCommandList()
{
    //清理当前信令列表
    vector<CClientCommandList*> vecClientCommandList;
    m_objClientCommandList.Get_All_Used(vecClientCommandList);

    uint32 u4Size = (uint32)vecClientCommandList.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        SAFE_DELETE(vecClientCommandList[i]);
    }

    m_objClientCommandList.Clear();
}

CClientCommandList* CMessageService::GetClientCommandList(uint16 u2CommandID)
{
    return m_objClientCommandList.Get_Hash_Box_Data_By_Uint32((uint32)u2CommandID);
}

bool CMessageService::DoMessage(ACE_Time_Value& tvBegin, IMessage* pMessage, uint16& u2CommandID, uint32& u4TimeCost, uint16& u2Count, bool& bDeleteFlag)
{
    if (NULL == pMessage)
    {
        OUR_DEBUG((LM_ERROR, "[CMessageService::DoMessage] pMessage is NULL.\n"));
        return false;
    }

    //放给需要继承的ClientCommand类去处理
    CClientCommandList* pClientCommandList = GetClientCommandList(u2CommandID);

    if (pClientCommandList != NULL)
    {
        int nCount = pClientCommandList->GetCount();

        for (int i = 0; i < nCount; i++)
        {
            _ClientCommandInfo* pClientCommandInfo = pClientCommandList->GetClientCommandIndex(i);

            if (NULL != pClientCommandInfo)
            {
                //判断当前消息是否有指定的监听端口
                if (pClientCommandInfo->m_objListenIPInfo.m_nPort > 0)
                {
                    if (ACE_OS::strcmp(pClientCommandInfo->m_objListenIPInfo.m_szClientIP, pMessage->GetMessageBase()->m_szListenIP) != 0 ||
                        (uint32)pClientCommandInfo->m_objListenIPInfo.m_nPort != pMessage->GetMessageBase()->m_u4ListenPort)
                    {
                        continue;
                    }
                }

                //标记当前命令运行状态
                pClientCommandInfo->m_pClientCommand->DoMessage(pMessage, bDeleteFlag);

                //这里指记录处理毫秒数
                ACE_Time_Value tvCost = ACE_OS::gettimeofday() - tvBegin;
                u4TimeCost = (uint32)tvCost.msec();

                //记录命令被调用次数
                u2Count++;
                //OUR_DEBUG((LM_ERROR, "[CMessageManager::DoMessage]u2CommandID = %d End.\n", u2CommandID));

            }
        }

        return true;
    }
    else
    {
        //没有找到对应的注册指令，如果不是define.h定义的异常，则记录异常命令日志
        if (CLIENT_LINK_CONNECT != u2CommandID  && CLIENT_LINK_CDISCONNET != u2CommandID &&
            CLIENT_LINK_SDISCONNET != u2CommandID  && CLINET_LINK_SENDTIMEOUT != u2CommandID &&
            CLINET_LINK_SENDERROR != u2CommandID  && CLINET_LINK_CHECKTIMEOUT != u2CommandID &&
            CLIENT_LINK_SENDOK != u2CommandID)
        {
            char szLog[MAX_BUFF_500] = { '\0' };
            sprintf_safe(szLog, MAX_BUFF_500, "[CommandID=%d][HeadLen=%d][BodyLen=%d] is not plugin dispose.",
                         u2CommandID,
                         pMessage->GetMessageBase()->m_u4HeadSrcSize,
                         pMessage->GetMessageBase()->m_u4BodySrcSize);
            AppLogManager::instance()->WriteLog(LOG_SYSTEM_ERROR, szLog);
        }
    }

    return false;
}

_ThreadInfo* CMessageService::GetThreadInfo()
{
    return &m_ThreadInfo;
}

void CMessageService::GetAIInfo(_WorkThreadAIInfo& objAIInfo)
{
    m_WorkThreadAI.GetAIInfo(objAIInfo);
}

uint32 CMessageService::GetThreadID()
{
    return m_u4ThreadID;
}

void CMessageService::CopyMessageManagerList()
{
    CloseCommandList();

    CHashTable<CClientCommandList>* pClientCommandList = App_MessageManager::instance()->GetHashCommandList();

    if (NULL != pClientCommandList)
    {
        vector<CClientCommandList*> vecClientCommandList;
        pClientCommandList->Get_All_Used(vecClientCommandList);

        uint32 u4Size = (uint32)vecClientCommandList.size();

        for (uint32 i = 0; i < u4Size; i++)
        {
            CClientCommandList* pClientCommandList = vecClientCommandList[i];

            if (NULL != pClientCommandList)
            {
                CClientCommandList* pCurrClientCommandList = new CClientCommandList();
                pCurrClientCommandList->SetCommandID(pClientCommandList->GetCommandID());

                for (int j = 0; j < pClientCommandList->GetCount(); j++)
                {
                    if (false == pCurrClientCommandList->AddClientCommand(pClientCommandList->GetClientCommandIndex(j)->m_pClientCommand, pClientCommandList->GetClientCommandIndex(j)->m_szModuleName))
                    {
                        OUR_DEBUG((LM_INFO, "[CMessageService::CopyMessageManagerList]<%s>CommandID=%d add error.\n", pClientCommandList->GetClientCommandIndex(j)->m_szModuleName, pClientCommandList->GetCommandID()));
                    }
                }

                if (false == m_objClientCommandList.Add_Hash_Data_By_Key_Unit32((uint32)pClientCommandList->GetCommandID(), pCurrClientCommandList))
                {
                    OUR_DEBUG((LM_INFO, "[CMessageService::CopyMessageManagerList]CommandID=%d add error.\n", pClientCommandList->GetCommandID()));
                }
            }
        }
    }
}

void CMessageService::GetAITO(vecCommandTimeout& objTimeout)
{
    m_WorkThreadAI.GetAllTimeout(m_u4ThreadID, objTimeout);
}

void CMessageService::GetAITF(vecCommandTimeout& objTimeout)
{
    m_WorkThreadAI.GetAllForbiden(m_u4ThreadID, objTimeout);
}

void CMessageService::SetAI(uint8 u1AI, uint32 u4DisposeTime, uint32 u4WTCheckTime, uint32 u4WTStopTime)
{
    m_WorkThreadAI.ReSet(u1AI, u4DisposeTime, u4WTCheckTime, u4WTStopTime);
}

_CommandData* CMessageService::GetCommandData(uint16 u2CommandID)
{
    return m_CommandAccount.GetCommandData(u2CommandID);
}

void CMessageService::GetFlowInfo(uint32& u4FlowIn, uint32& u4FlowOut)
{
    u4FlowIn  = m_CommandAccount.GetFlowIn();
    u4FlowOut = m_CommandAccount.GetFlowOut();
}

void CMessageService::GetCommandAlertData(vecCommandAlertData& CommandAlertDataList)
{
    m_CommandAccount.GetCommandAlertData(CommandAlertDataList);
}

void CMessageService::SaveCommandDataLog()
{
    m_CommandAccount.SaveCommandDataLog();
}

void CMessageService::SetThreadState(MESSAGE_SERVICE_THREAD_STATE emState)
{
    m_emThreadState = emState;
}

MESSAGE_SERVICE_THREAD_STATE CMessageService::GetThreadState()
{
    return m_emThreadState;
}

uint32 CMessageService::GetStepState()
{
    return m_ThreadInfo.m_u4State;
}

uint32 CMessageService::GetUsedMessageCount()
{
    return (uint32)m_MessagePool.GetUsedCount();
}

CMessage* CMessageService::CreateMessage()
{
    //OUR_DEBUG((LM_INFO, "[CMessageService::CreateMessage]GetThreadID=%d, m_MessagePool=0x%08x.\n", GetThreadID(), m_MessagePool));
    CMessage* pMessage = m_MessagePool.Create();

    if(NULL != pMessage)
    {
        pMessage->GetMessageBase()->m_u4WorkThreadID = GetThreadID();
    }

    return pMessage;
}

void CMessageService::DeleteMessage(CMessage* pMessage)
{
    //OUR_DEBUG((LM_INFO, "[CMessageService::DeleteMessage]GetThreadID=%d, m_MessagePool=0x%08x.\n", GetThreadID(), m_MessagePool));
    if (false == m_MessagePool.Delete(pMessage))
    {
        OUR_DEBUG((LM_INFO, "[CMessageService::DeleteMessage]pMessage == NULL.\n"));
    }
}

void CMessageService::GetFlowPortList(vector<_Port_Data_Account>& vec_Port_Data_Account)
{
    m_CommandAccount.GetFlowPortList(vec_Port_Data_Account);
}

int CMessageService::handle_signal(int signum, siginfo_t* siginfo, ucontext_t* ucontext)
{
    if (signum == SIGUSR1 + grp_id())
    {
        OUR_DEBUG((LM_INFO,"[CMessageService::handle_signal](%d) will be kill.\n", grp_id()));

        if(NULL != siginfo && NULL != ucontext)
        {
            OUR_DEBUG((LM_INFO,"[CMessageService::handle_signal]siginfo is not null.\n"));
        }

        ACE_Thread::exit();
    }

    return 0;
}

int CMessageService::CloseMsgQueue()
{
    // We can choose to process the message or to differ it into the message
    // queue, and process them into the svc() method. Chose the last option.
    int retval;

    ACE_Message_Block* mblk = 0;
    ACE_NEW_RETURN(mblk,ACE_Message_Block (0, ACE_Message_Block::MB_STOP),-1);

    // If queue is full, flush it before block in while
    if (msg_queue ()->is_full())
    {
        if ((retval=msg_queue ()->flush()) == -1)
        {
            OUR_DEBUG((LM_ERROR, "[CMessageService::CloseMsgQueue]put error flushing queue\n"));
            return -1;
        }
    }

    m_mutex.acquire();

    while ((retval = putq (mblk)) == -1)
    {
        if (msg_queue ()->state () != ACE_Message_Queue_Base::PULSED)
        {
            OUR_DEBUG((LM_ERROR,ACE_TEXT("[CMessageService::CloseMsgQueue]put Queue not activated.\n")));
            break;
        }
    }

    m_cond.wait();
    m_mutex.release();
    return retval;
}

//==========================================================
CMessageServiceGroup::CMessageServiceGroup()
{
    m_u4TimerID      = 0;
    m_u4MaxQueue     = 0;
    m_u4HighMask     = 0;
    m_u4LowMask      = 0;
    m_u2CurrThreadID = 0;

    uint16 u2ThreadTimeCheck = App_MainConfig::instance()->GetThreadTimeCheck();

    if(u2ThreadTimeCheck == 0)
    {
        m_u2ThreadTimeCheck = MAX_MSG_TIMEDELAYTIME;
    }
    else
    {
        m_u2ThreadTimeCheck = u2ThreadTimeCheck;
    }
}

CMessageServiceGroup::~CMessageServiceGroup()
{
    //Close();
}

int CMessageServiceGroup::handle_timeout(const ACE_Time_Value& tv, const void* arg)
{
    ACE_UNUSED_ARG(arg);
    ACE_UNUSED_ARG(tv);

    //检查所有工作线程
    if (false == CheckWorkThread())
    {
        OUR_DEBUG((LM_ERROR, "[CMessageServiceGroup::handle_timeout]CheckWorkThread is fail.\n"));
    }

    //记录PacketParse的统计过程
    if (false == CheckPacketParsePool())
    {
        OUR_DEBUG((LM_ERROR, "[CMessageServiceGroup::handle_timeout]CheckPacketParsePool is fail.\n"));
    }

    //记录统计CPU和内存的使用
    if (false == CheckCPUAndMemory())
    {
        OUR_DEBUG((LM_ERROR, "[CMessageServiceGroup::handle_timeout]CheckCPUAndMemory is fail.\n"));
    }

    //检查所有插件状态
    if (false == CheckPlugInState())
    {
        OUR_DEBUG((LM_ERROR, "[CMessageServiceGroup::handle_timeout]CheckPlugInState is fail.\n"));
    }

    //存入工作线程Json图表
    SaveThreadInfoJson();

    //存储当前连接Json图表
    SaveConnectJson(tv);

    //存储命令Json图表
    SaveCommandChart(tv);

    return 0;
}

bool CMessageServiceGroup::Init(uint32 u4ThreadCount, uint32 u4MaxQueue, uint32 u4LowMask, uint32 u4HighMask)
{
    //删除以前的所有CMessageService对象
    //Close();

    //记录当前设置
    m_u4MaxQueue     = u4MaxQueue;
    m_u4HighMask     = u4HighMask;
    m_u4LowMask      = u4LowMask;
    m_u2CurrThreadID = 0;

    m_objAllThreadInfo.Init((int)u4ThreadCount);

    //时序模式开启
    OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::Init]Timing sequence Start.\n"));

    //初始化所有的Message对象
    for (uint32 i = 0; i < u4ThreadCount; i++)
    {
        CMessageService* pMessageService = new CMessageService();

        if (NULL == pMessageService)
        {
            OUR_DEBUG((LM_ERROR, "[CMessageServiceGroup::Init](%d)pMessageService is NULL.\n", i));
            return false;
        }

        pMessageService->Init(i, u4MaxQueue, u4LowMask, u4HighMask);

        m_vecMessageService.push_back(pMessageService);
    }

    //初始化连接信息历史记录
    if (true == App_MainConfig::instance()->GetConnectChart()->m_blJsonOutput)
    {
        m_objConnectHistoryList.Init(App_MainConfig::instance()->GetConnectChart()->m_u2Count);
    }

    //初始化命令图表记录
    uint32 u4Count = App_MainConfig::instance()->GetCommandChartCount();

    for (uint32 i = 0; i < u4Count; i++)
    {
        CObjectLruList<_Command_Chart_Info, ACE_Null_Mutex> obj_Command_Chart_Info;
        obj_Command_Chart_Info.Init(App_MainConfig::instance()->GetCommandChart(i)->m_u2Count);
        m_vec_Command_Chart_Info.push_back(obj_Command_Chart_Info);
    }

    return true;
}

bool CMessageServiceGroup::PutMessage(CMessage* pMessage)
{
    //判断是否需要数据染色
    string strTraceID = m_objMessageDyeingManager.GetTraceID(pMessage->GetMessageBase()->m_szIP,
                        (short)pMessage->GetMessageBase()->m_u4Port,
                        pMessage->GetMessageBase()->m_u2Cmd);

    if (strTraceID.length() > 0)
    {
        //需要染色，生成TraceID
        sprintf_safe(pMessage->GetMessageBase()->m_szTraceID, MAX_BUFF_50, "%s", strTraceID.c_str());
    }

    //判断是否为TCP包，如果是则按照ConnectID区分。UDP则随机分配一个
    int32 n4ThreadID = 0;

    //得到工作线程ID
    n4ThreadID = pMessage->GetMessageBase()->m_u4WorkThreadID;

    if (-1 == n4ThreadID)
    {
        pMessage->Clear();
        SAFE_DELETE(pMessage);
        return false;
    }

    CMessageService* pMessageService = m_vecMessageService[(uint32)n4ThreadID];

    if (NULL != pMessageService)
    {
        if (false == pMessageService->PutMessage(pMessage))
        {
            OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::PutMessage](%d)pMessageService fail.\n", pMessageService->GetThreadID()));
        }
    }

    return true;
}

bool CMessageServiceGroup::PutUpdateCommandMessage(uint32 u4UpdateIndex)
{
    //向所有工作线程群发副本更新消息

    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService)
        {
            if (false == pMessageService->PutUpdateCommandMessage(u4UpdateIndex))
            {
                OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::PutMessage](%d)pMessageService fail.\n", pMessageService->GetThreadID()));
                return false;
            }
        }
    }

    return true;
}

void CMessageServiceGroup::Close()
{
    if (false == KillTimer())
    {
        OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::Close]KillTimer error.\n"));
    }

    ACE_Time_Value tvSleep(0, 1000);

    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService && 0 != pMessageService->Close())
        {
            OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::Close](%d)pMessageService fail.\n", pMessageService->GetThreadID()));
        }

        ACE_OS::sleep(tvSleep);
        SAFE_DELETE(pMessageService);
    }

    m_vecMessageService.clear();
}

bool CMessageServiceGroup::Start()
{
    if(StartTimer() == false)
    {
        return false;
    }

    OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::Start]Work thread count=%d.\n", m_vecMessageService.size()));

    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService && false == pMessageService->Start())
        {
            OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::Start](%d)WorkThread is fail.\n", i));
            return false;
        }

        OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::Start](%d)WorkThread is OK.\n", i));
    }

    return true;
}

bool CMessageServiceGroup::StartTimer()
{
    OUR_DEBUG((LM_ERROR, "[CMessageServiceGroup::StartTimer] begin....\n"));

    m_u4TimerID = App_TimerManager::instance()->schedule(this, NULL, ACE_OS::gettimeofday() + ACE_Time_Value(MAX_MSG_STARTTIME), ACE_Time_Value(m_u2ThreadTimeCheck));

    if(0 == m_u4TimerID)
    {
        OUR_DEBUG((LM_ERROR, "[CMessageServiceGroup::StartTimer] Start thread time error.\n"));
        return false;
    }

    return true;
}

bool CMessageServiceGroup::KillTimer()
{
    OUR_DEBUG((LM_ERROR, "[CMessageServiceGroup::KillTimer] begin....\n"));

    if(m_u4TimerID > 0)
    {
        App_TimerManager::instance()->cancel(m_u4TimerID);
        m_u4TimerID = 0;
    }

    OUR_DEBUG((LM_ERROR, "[CMessageServiceGroup::KillTimer] end....\n"));
    return true;
}

bool CMessageServiceGroup::SaveCommandChart(ACE_Time_Value tvNow)
{
    char szDataXInfo[MAX_BUFF_500]  = { '\0' };
    char szDataYInfo[MAX_BUFF_200]  = { '\0' };
    char szDataXLine[MAX_BUFF_200]  = { '\0' };
    char szDataYLine[MAX_BUFF_500]  = { '\0' };
    char szDataJson[MAX_BUFF_1024]  = { '\0' };
    uint32 u4Count = App_MainConfig::instance()->GetCommandChartCount();

    for (uint32 i = 0; i < u4Count; i++)
    {
        _Command_Chart_Info obj_Command_Chart_Info;
        _CommandData objCommandData;

        obj_Command_Chart_Info.m_u2CommandID    = App_MainConfig::instance()->GetCommandChart(i)->m_u4CommandID;
        obj_Command_Chart_Info.m_tvCommandTime  = tvNow;
        GetCommandData(obj_Command_Chart_Info.m_u2CommandID, objCommandData);
        obj_Command_Chart_Info.m_u4CommandCount = objCommandData.m_u4CommandCount;
        m_vec_Command_Chart_Info[i].AddObject(obj_Command_Chart_Info);

        //输出成图表数据
        //生成X轴和Y轴
        vector<_Command_Chart_Info> objVecHistoryList;
        m_vec_Command_Chart_Info[i].GetAllSavingObject(objVecHistoryList);

        for (int j = 0; j < (int)objVecHistoryList.size(); j++)
        {
            ACE_Date_Time dtThreadTime(objVecHistoryList[j].m_tvCommandTime);

            if (0 == j)
            {
                sprintf_safe(szDataXInfo, MAX_BUFF_500, "\"%02d:%02d:%02d\"",
                             dtThreadTime.hour(),
                             dtThreadTime.minute(),
                             dtThreadTime.second());

                sprintf_safe(szDataYInfo, MAX_BUFF_200, "%d",
                             objVecHistoryList[i].m_u4CommandCount);
            }
            else
            {
                char szTemp[MAX_BUFF_500] = { '\0' };
                sprintf_safe(szTemp, MAX_BUFF_500, "%s", szDataXInfo);
                sprintf_safe(szDataXInfo, MAX_BUFF_500, "%s,\"%02d:%02d:%02d\"",
                             szTemp,
                             dtThreadTime.hour(),
                             dtThreadTime.minute(),
                             dtThreadTime.second());
                sprintf_safe(szTemp, MAX_BUFF_200, "%s", szDataYInfo);
                sprintf_safe(szDataYInfo, MAX_BUFF_200, "%s, %d",
                             szTemp,
                             objVecHistoryList[i].m_u4CommandCount);
            }

            sprintf_safe(szDataXLine, MAX_BUFF_200, OUTPUT_CHART_JSON_X, szDataXInfo);
            sprintf_safe(szDataYLine, MAX_BUFF_500, OUTPUT_CHART_JSON_Y, objVecHistoryList[i].m_u2CommandID, szDataYInfo);
        }

        //拼接成数据
        char szTableName[MAX_BUFF_200] = { '\0' };
        sprintf_safe(szTableName, MAX_BUFF_200, "Pss Command(0x%04x)", obj_Command_Chart_Info.m_u2CommandID);
        sprintf_safe(szDataJson, MAX_BUFF_1024, OUTPUT_CHART_JSON, szTableName, szDataXLine, szDataYLine);
        FILE* pFile = ACE_OS::fopen(App_MainConfig::instance()->GetCommandChart(i)->m_szJsonFile, "w");

        if (NULL != pFile)
        {
            ACE_OS::fwrite(szDataJson, sizeof(char), strlen(szDataJson), pFile);
            ACE_OS::fclose(pFile);
        }
    }

    return true;
}

bool CMessageServiceGroup::SaveThreadInfoJson()
{
    char  szJsonContent[MAX_BUFF_1024] = { '\0' };
    char  szYLineData[MAX_BUFF_100]    = { '\0' };
    char  szYLinesData[MAX_BUFF_500]   = { '\0' };
    char  szXLinesName[MAX_BUFF_500]   = { '\0' };

    if (false == App_MainConfig::instance()->GetWorkThreadChart()->m_blJsonOutput)
    {
        return true;
    }

    char* pJsonFile = App_MainConfig::instance()->GetWorkThreadChart()->m_szJsonFile;

    //获得当前线程数量
    uint32 u4Size = (uint32)m_vecMessageService.size();

    if (NULL != pJsonFile && 0 < ACE_OS::strlen(pJsonFile) && u4Size > 0)
    {
        //得到X轴描述
        m_vecMessageService[0]->GetThreadInfoTimeJson(szXLinesName, MAX_BUFF_500);

        //获得Y轴描述
        for (uint32 i = 0; i < u4Size; i++)
        {
            CMessageService* pMessageService = m_vecMessageService[i];

            if (NULL != pMessageService)
            {
                pMessageService->GetThreadInfoJson(szYLineData, MAX_BUFF_500);
            }

            if (0 == i)
            {
                sprintf_safe(szYLinesData, MAX_BUFF_500, "%s", szYLineData);
            }
            else
            {
                char szTemp[MAX_BUFF_500] = { '\0' };
                sprintf_safe(szTemp, MAX_BUFF_500, "%s", szYLinesData);
                sprintf_safe(szYLinesData, MAX_BUFF_500, "%s,%s", szTemp, szYLineData);
            }
        }

        sprintf_safe(szJsonContent, MAX_BUFF_1024, OUTPUT_CHART_JSON, "PSS Work Thread", szXLinesName, szYLinesData);
        FILE* pFile = ACE_OS::fopen(pJsonFile, "w");

        if (NULL != pFile)
        {
            ACE_OS::fwrite(szJsonContent, sizeof(char), strlen(szJsonContent), pFile);
            ACE_OS::fclose(pFile);
        }
    }

    return true;
}

bool CMessageServiceGroup::SaveConnectJson(ACE_Time_Value tvNow)
{
    _Connect_Chart_Info obj_Connect_Chart_Info;
#ifdef WIN32
    obj_Connect_Chart_Info.m_n4ConnectCount     = App_ProConnectManager::instance()->GetCount();
    obj_Connect_Chart_Info.m_tvConnectTime      = tvNow;
    obj_Connect_Chart_Info.m_u4LastConnectCount = App_IPAccount::instance()->GetLastConnectCount();
#else
    obj_Connect_Chart_Info.m_n4ConnectCount     = App_ConnectManager::instance()->GetCount();
    obj_Connect_Chart_Info.m_tvConnectTime      = tvNow;
    obj_Connect_Chart_Info.m_u4LastConnectCount = App_IPAccount::instance()->GetLastConnectCount();
#endif
    m_objConnectHistoryList.AddObject(obj_Connect_Chart_Info);

    //写入文件
    char  szJsonContent[MAX_BUFF_1024] = { '\0' };
    char  szYLineData[MAX_BUFF_200]    = { '\0' };
    char  szYLineDataC[MAX_BUFF_200]   = { '\0' };
    char  szYLinesData[MAX_BUFF_500]   = { '\0' };
    char  szXLinesName[MAX_BUFF_500]   = { '\0' };

    if (false == App_MainConfig::instance()->GetConnectChart()->m_blJsonOutput)
    {
        return true;
    }

    char* pJsonFile = App_MainConfig::instance()->GetConnectChart()->m_szJsonFile;

    if (NULL != pJsonFile && 0 < ACE_OS::strlen(pJsonFile))
    {
        //得到X轴描述
        GetConnectTimeJson(szXLinesName, MAX_BUFF_500);

        //得到Y轴描述(当前活跃连接数)
        GetConnectJson(szYLineData, MAX_BUFF_200);

        //得到Y轴描述(当前每分钟连接数)
        GetCurrConnectJson(szYLineDataC, MAX_BUFF_200);

        sprintf_safe(szYLinesData, MAX_BUFF_500, "%s,%s", szYLineData, szYLineDataC);

        sprintf_safe(szJsonContent, MAX_BUFF_1024, OUTPUT_CHART_JSON, "PSS Connect", szXLinesName, szYLinesData);
        FILE* pFile = ACE_OS::fopen(pJsonFile, "w");

        if (NULL != pFile)
        {
            ACE_OS::fwrite(szJsonContent, sizeof(char), strlen(szJsonContent), pFile);
            ACE_OS::fclose(pFile);
        }
    }

    return true;
}

bool CMessageServiceGroup::CheckWorkThread()
{
    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService && false == pMessageService->SaveThreadInfoData())
        {
            OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::CheckWorkThread]SaveThreadInfo error.\n"));
        }

        //目前在工作线程发生阻塞的时候不在杀死工程线程，杀了工作线程会导致一些内存问题。
    }

    return true;
}

bool CMessageServiceGroup::CheckPacketParsePool()
{
    AppLogManager::instance()->WriteLog(LOG_SYSTEM_PACKETTHREAD, "[CMessageService::handle_timeout] UsedCount = %d, FreeCount= %d.", App_PacketParsePool::instance()->GetUsedCount(), App_PacketParsePool::instance()->GetFreeCount());
    return true;
}

bool CMessageServiceGroup::CheckCPUAndMemory()
{
    if (App_MainConfig::instance()->GetMonitor() == 1)
    {
#ifdef WIN32
        uint32 u4CurrCpu = (uint32)GetProcessCPU_Idel();
        //uint32 u4CurrMemory = (uint32)GetProcessMemorySize();
#else
        uint32 u4CurrCpu = (uint32)GetProcessCPU_Idel_Linux();
        //uint32 u4CurrMemory = (uint32)GetProcessMemorySize_Linux();
#endif

        //获得相关Messageblock,BuffPacket,MessageCount,内存大小
        uint32 u4MessageBlockUsedSize = App_MessageBlockManager::instance()->GetUsedSize();
        uint32 u4BuffPacketCount = App_BuffPacketManager::instance()->GetBuffPacketUsedCount();
        uint32 u4MessageCount = GetUsedMessageCount();

        if (u4CurrCpu > App_MainConfig::instance()->GetCpuMax() || u4MessageBlockUsedSize > App_MainConfig::instance()->GetMemoryMax())
        {
            OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::handle_timeout]CPU Rote=%d,MessageBlock=%d,u4BuffPacketCount=%d,u4MessageCount=%d ALERT.\n", u4CurrCpu, u4MessageBlockUsedSize, u4BuffPacketCount, u4MessageCount));
            AppLogManager::instance()->WriteLog(LOG_SYSTEM_MONITOR, "[Monitor] CPU Rote=%d,MessageBlock=%d,u4BuffPacketCount=%d,u4MessageCount=%d.", u4CurrCpu, u4MessageBlockUsedSize, u4BuffPacketCount, u4MessageCount);
        }
        else
        {
            OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::handle_timeout]CPU Rote=%d,MessageBlock=%d,u4BuffPacketCount=%d,u4MessageCount=%d OK.\n", u4CurrCpu, u4MessageBlockUsedSize, u4BuffPacketCount, u4MessageCount));
        }
    }

    return true;
}

bool CMessageServiceGroup::CheckPlugInState()
{
    vector<_ModuleInfo*> vecModeInfo;
    App_ModuleLoader::instance()->GetAllModuleInfo(vecModeInfo);

    uint32 u4Size = (uint32)vecModeInfo.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        _ModuleInfo* pModuleInfo = vecModeInfo[i];

        if (NULL != pModuleInfo)
        {
            uint32 u4ErrorID = 0;
            bool blModuleState = pModuleInfo->GetModuleState(u4ErrorID);

            if (false == blModuleState)
            {
                char szTitle[MAX_BUFF_50] = { '\0' };
                sprintf_safe(szTitle, MAX_BUFF_50, "ModuleStateError");

                //发送邮件
                AppLogManager::instance()->WriteToMail(LOG_SYSTEM_MONITOR, 1,
                                                       szTitle,
                                                       "Module ErrorID=%d.\n",
                                                       u4ErrorID);
            }
        }
    }

    return true;
}

void CMessageServiceGroup::AddDyringIP(const char* pClientIP, uint16 u2MaxCount)
{
    return m_objMessageDyeingManager.AddDyringIP(pClientIP, u2MaxCount);
}

bool CMessageServiceGroup::AddDyeingCommand(uint16 u2CommandID, uint16 u2MaxCount)
{
    return m_objMessageDyeingManager.AddDyeingCommand(u2CommandID, u2MaxCount);
}

void CMessageServiceGroup::GetDyeingCommand(vec_Dyeing_Command_list& objList)
{
    m_objMessageDyeingManager.GetDyeingCommand(objList);
}

void CMessageServiceGroup::GetFlowPortList(vector<_Port_Data_Account>& vec_Port_Data_Account)
{
    vec_Port_Data_Account.clear();
    vector<_Port_Data_Account> vec_Service_Port_Data_Account;

    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService)
        {
            pMessageService->GetFlowPortList(vec_Service_Port_Data_Account);

            Combo_Port_List(vec_Service_Port_Data_Account, vec_Port_Data_Account);
        }
    }
}

bool CMessageServiceGroup::GetConnectJson(char* pJson, uint32 u4Len)
{
    //将数据输出成Json格式数据
    char szDataInfo[MAX_BUFF_200] = { '\0' };
    vector<_Connect_Chart_Info> objVecHistoryList;
    m_objConnectHistoryList.GetAllSavingObject(objVecHistoryList);

    for (int i = 0; i < (int)objVecHistoryList.size(); i++)
    {
        if (0 == i)
        {
            sprintf_safe(szDataInfo, MAX_BUFF_200, "%d",
                         objVecHistoryList[i].m_n4ConnectCount);
        }
        else
        {
            char szTemp[MAX_BUFF_200] = { '\0' };
            sprintf_safe(szTemp, MAX_BUFF_200, "%s", szDataInfo);
            sprintf_safe(szDataInfo, MAX_BUFF_200, "%s, %d",
                         szTemp,
                         objVecHistoryList[i].m_n4ConnectCount);
        }
    }

    sprintf_safe(pJson, u4Len, OUTPUT_CHART_JSON_Y, 0, szDataInfo);

    return true;
}

bool CMessageServiceGroup::GetCurrConnectJson(char* pJson, uint32 u4Len)
{
    //将数据输出成Json格式数据
    char szDataInfo[MAX_BUFF_200] = { '\0' };
    vector<_Connect_Chart_Info> objVecHistoryList;
    m_objConnectHistoryList.GetAllSavingObject(objVecHistoryList);

    for (int i = 0; i < (int)objVecHistoryList.size(); i++)
    {
        if (0 == i)
        {
            sprintf_safe(szDataInfo, MAX_BUFF_200, "%d",
                         objVecHistoryList[i].m_u4LastConnectCount);
        }
        else
        {
            char szTemp[MAX_BUFF_200] = { '\0' };
            sprintf_safe(szTemp, MAX_BUFF_200, "%s", szDataInfo);
            sprintf_safe(szDataInfo, MAX_BUFF_200, "%s, %d",
                         szTemp,
                         objVecHistoryList[i].m_u4LastConnectCount);
        }
    }

    sprintf_safe(pJson, u4Len, OUTPUT_CHART_JSON_Y, 1, szDataInfo);

    return true;
}

bool CMessageServiceGroup::GetConnectTimeJson(char* pJson, uint32 u4Len)
{
    //将数据输出成Json格式数据
    char szDataInfo[MAX_BUFF_500] = { '\0' };
    vector<_Connect_Chart_Info> objVecHistoryList;
    m_objConnectHistoryList.GetAllSavingObject(objVecHistoryList);

    for (int i = 0; i < (int)objVecHistoryList.size(); i++)
    {
        ACE_Date_Time dtThreadTime(objVecHistoryList[i].m_tvConnectTime);

        if (0 == i)
        {
            sprintf_safe(szDataInfo, MAX_BUFF_500, "\"%02d:%02d:%02d\"",
                         dtThreadTime.hour(),
                         dtThreadTime.minute(),
                         dtThreadTime.second());
        }
        else
        {
            char szTemp[MAX_BUFF_500] = { '\0' };
            sprintf_safe(szTemp, MAX_BUFF_500, "%s", szDataInfo);
            sprintf_safe(szDataInfo, MAX_BUFF_500, "%s,\"%02d:%02d:%02d\"",
                         szTemp,
                         dtThreadTime.hour(),
                         dtThreadTime.minute(),
                         dtThreadTime.second());
        }
    }

    sprintf_safe(pJson, u4Len, OUTPUT_CHART_JSON_X, szDataInfo);

    return true;
}

CThreadInfo* CMessageServiceGroup::GetThreadInfo()
{
    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService)
        {
            _ThreadInfo* pThreadInfo = pMessageService->GetThreadInfo();

            if (NULL != pThreadInfo)
            {
                m_objAllThreadInfo.AddThreadInfo(i, pThreadInfo);
            }
        }
    }

    return &m_objAllThreadInfo;
}

uint32 CMessageServiceGroup::GetUsedMessageCount()
{
    uint32 u4Count = 0;

    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        u4Count += m_vecMessageService[i]->GetUsedMessageCount();
    }

    return u4Count;
}

uint32 CMessageServiceGroup::GetWorkThreadCount()
{
    return (uint32)m_vecMessageService.size();
}

uint32 CMessageServiceGroup::GetWorkThreadIDByIndex(uint32 u4Index)
{
    if(u4Index >= m_vecMessageService.size())
    {
        return (uint32)0;
    }
    else
    {
        return m_vecMessageService[u4Index]->GetThreadInfo()->m_u4ThreadID;
    }
}

void CMessageServiceGroup::GetWorkThreadAIInfo(vecWorkThreadAIInfo& objvecWorkThreadAIInfo)
{
    objvecWorkThreadAIInfo.clear();

    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        _WorkThreadAIInfo objWorkThreadAIInfo;
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService)
        {
            pMessageService->GetAIInfo(objWorkThreadAIInfo);
            objWorkThreadAIInfo.m_u4ThreadID = pMessageService->GetThreadID();
            objvecWorkThreadAIInfo.push_back(objWorkThreadAIInfo);
        }
    }
}

void CMessageServiceGroup::GetAITO(vecCommandTimeout& objTimeout)
{
    objTimeout.clear();

    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService)
        {
            pMessageService->GetAITO(objTimeout);
        }
    }
}

void CMessageServiceGroup::GetAITF(vecCommandTimeout& objTimeout)
{
    objTimeout.clear();

    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService)
        {
            pMessageService->GetAITF(objTimeout);
        }
    }
}

void CMessageServiceGroup::SetAI(uint8 u1AI, uint32 u4DisposeTime, uint32 u4WTCheckTime, uint32 u4WTStopTime)
{
    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService)
        {
            pMessageService->SetAI(u1AI, u4DisposeTime, u4WTCheckTime, u4WTStopTime);
        }
    }
}

void CMessageServiceGroup::GetCommandData(uint16 u2CommandID, _CommandData& objCommandData)
{
    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService)
        {
            _CommandData* pCommandData = pMessageService->GetCommandData(u2CommandID);

            if (NULL != pCommandData)
            {
                objCommandData += (*pCommandData);
            }
        }
    }
}

void CMessageServiceGroup::GetFlowInfo(uint32& u4FlowIn, uint32& u4FlowOut)
{
    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        uint32 u4CurrFlowIn  = 0;
        uint32 u4CurrFlowOut = 0;
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService)
        {
            pMessageService->GetFlowInfo(u4CurrFlowIn, u4CurrFlowOut);
            u4FlowIn  += u4CurrFlowIn;
            u4FlowOut += u4CurrFlowOut;
        }
    }
}

void CMessageServiceGroup::GetCommandAlertData(vecCommandAlertData& CommandAlertDataList)
{
    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService)
        {
            pMessageService->GetCommandAlertData(CommandAlertDataList);
        }
    }
}

void CMessageServiceGroup::SaveCommandDataLog()
{
    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL != pMessageService)
        {
            pMessageService->SaveCommandDataLog();
        }
    }
}

CMessage* CMessageServiceGroup::CreateMessage(uint32 u4ConnectID, uint8 u1PacketType)
{
    int32 n4ThreadID = 0;
    n4ThreadID = GetWorkThreadID(u4ConnectID, u1PacketType);

    if (-1 == n4ThreadID)
    {
        return NULL;
    }

    //OUR_DEBUG((LM_INFO, "[CMessageServiceGroup::CreateMessage]n4ThreadID=%d.\n", n4ThreadID));

    CMessageService* pMessageService = m_vecMessageService[(uint32)n4ThreadID];

    if (NULL != pMessageService)
    {
        return pMessageService->CreateMessage();
    }
    else
    {
        return NULL;
    }
}

void CMessageServiceGroup::DeleteMessage(uint32 u4ConnectID, CMessage* pMessage)
{
    ACE_UNUSED_ARG(u4ConnectID);

    int32 n4ThreadID = 0;
    n4ThreadID = pMessage->GetMessageBase()->m_u4WorkThreadID;

    if (-1 == n4ThreadID)
    {
        pMessage->Clear();
        SAFE_DELETE(pMessage);
        return;
    }

    CMessageService* pMessageService = m_vecMessageService[(uint32)n4ThreadID];

    if (NULL != pMessageService)
    {
        pMessageService->DeleteMessage(pMessage);
    }
}

void CMessageServiceGroup::CopyMessageManagerList()
{
    //初始化所有的Message对象
    uint32 u4Size = (uint32)m_vecMessageService.size();

    for (uint32 i = 0; i < u4Size; i++)
    {
        CMessageService* pMessageService = m_vecMessageService[i];

        if (NULL == pMessageService)
        {
            OUR_DEBUG((LM_ERROR, "[CMessageServiceGroup::CopyMessageManagerList](%d)pMessageService is NULL.\n", i));
        }
        else
        {
            pMessageService->CopyMessageManagerList();
        }
    }
}

int32 CMessageServiceGroup::GetWorkThreadID(uint32 u4ConnectID, uint8 u1PackeType)
{
    int32 n4ThreadID = -1;

    if(m_vecMessageService.size() == 0)
    {
        return n4ThreadID;
    }

    if(u1PackeType == PACKET_TCP)
    {
        n4ThreadID = u4ConnectID % (uint32)m_vecMessageService.size();
    }
    else if(u1PackeType == PACKET_UDP)
    {
        //如果是UDP协议，则记录当前线程的位置，直接+1，调用随机数速度比较慢（因为要读文件）
        m_ThreadLock.acquire();
        n4ThreadID = m_u2CurrThreadID;

        //当前m_u2CurrThreadID指向下一个线程ID
        if (m_u2CurrThreadID >= m_objAllThreadInfo.GetThreadCount() - 1)
        {
            m_u2CurrThreadID = 0;
        }
        else
        {
            m_u2CurrThreadID++;
        }

        m_ThreadLock.release();
    }

    return n4ThreadID;
}

