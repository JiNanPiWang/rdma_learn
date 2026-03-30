#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h> // 必须包含这个头文件

static void cleanup_rdma_resources(struct ibv_device **dev_list,
                                   struct ibv_context *ctx,
                                   struct ibv_pd *pd,
                                   struct ibv_cq *cq,
                                   struct ibv_qp *qp,
                                   struct ibv_mr *mr,
                                   char *buf)
{
    if (mr)
        ibv_dereg_mr(mr);
    if (buf)
        free(buf);
    if (qp)
        ibv_destroy_qp(qp);
    if (cq)
        ibv_destroy_cq(cq);
    if (pd)
        ibv_dealloc_pd(pd);
    if (ctx)
        ibv_close_device(ctx);
    if (dev_list)
        ibv_free_device_list(dev_list);
}

int modify_qp_to_rts(struct ibv_qp *qp)
{
    struct ibv_qp_attr attr;
    union ibv_gid local_gid;
    int flags;
    int ret;
    const int port_num = 1;
    const int gid_index = 3;

    // 参数保护：避免空指针导致段错误
    if (!qp)
    {
        fprintf(stderr, "modify_qp_to_rts: QP 为空\n");
        return 1;
    }

    // GID = Global Identifier（全局标识符），可以理解为 RDMA 世界里的“IP 地址”。
    // 在 RoCE v2 场景，QP 进入 RTR 时通常需要一条可路由的全局地址（通过 GRH 携带）。
    // Loopback(自发自收) 时，我们把“目标 GID”设置为本机 GID。
    // 这样 dest_qp_num 指向自己时，报文路径和目标地址都能闭环回到本机。
    memset(&local_gid, 0, sizeof(local_gid));
    ret = ibv_query_gid(qp->context, port_num, gid_index, &local_gid);
    if (ret)
    {
        fprintf(stderr, "查询 GID 失败 (port=%d, gid_index=%d)\n", port_num, gid_index);
        return 1;
    }

    // 1) RESET -> INIT
    // QP 创建后默认在 RESET，硬件规定的状态机顺序是：RESET -> INIT -> RTR -> RTS。
    // INIT 阶段只做本地初始化：端口、PKey、访问权限等，还没建立远端收发路径。
    memset(&attr, 0, sizeof(attr));
    // attr 是“本次状态切换要写入硬件的参数包”，每个阶段都会重新清零再填充。
    attr.qp_state = IBV_QPS_INIT; // 目标状态：INIT
    attr.port_num = port_num;     // 绑定物理端口，通常是 1
    attr.pkey_index = 0;          // 分区键索引，默认分区一般用 0
    // 这条 QP 允许哪些 RDMA 访问能力（本地写 / 远端读 / 远端写）。
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    // flags 是“字段有效位掩码”：告诉 ibv_modify_qp，attr 里哪些字段要真正生效。
    // 这里表示：我要同时设置 QP 状态、PKey 索引、端口号、访问权限。
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    // 注意：能否成功进入 INIT，不是由 flags 本身判断。
    // 真正判断标准是 ibv_modify_qp 返回值：0 成功，非 0 失败。
    if (ibv_modify_qp(qp, &attr, flags))
    {
        fprintf(stderr, "将 QP 切换到 INIT 失败\n");
        return 1;
    }

    // 2) INIT -> RTR (Ready to Receive)
    // RTR = 接收路径就绪。此时重点是把 RQ 相关能力和“对端信息”配置完整。
    // 为什么先 RTR 再 RTS？因为 RC 协议要求“先确保接收端准备好”，再允许发送。
    // 如果先发后收，可靠传输语义无法保证，硬件状态机会拒绝。
    // 虽然你可以把 RQ/SQ 理解成两条队列，但它们同属于一个 QP 对象，
    // 所以状态是“QP 级别”推进，而不是给 SQ/RQ 分别设置独立状态。
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;   // 目标状态：RTR
    attr.path_mtu = IBV_MTU_1024;  // path_mtu：本连接允许的最大报文大小上限（影响分片/吞吐）
    attr.dest_qp_num = qp->qp_num; // 目标 QP 号（Loopback 时对端就是自己）
    attr.rq_psn = 0;               // 接收方向期望的起始 PSN（包序号）
    attr.max_dest_rd_atomic = 1;   // 允许对端同时发起到本端的 RDMA Read/Atomic 数量
    attr.min_rnr_timer = 12;       // RNR NAK 后最小等待时间（防止接收队列来不及）

    // AH(Address Handle) 是“怎么把包送到目标”的地址参数集合。
    // RoCE v2 常见做法是启用 GRH 并填好 SGID/DGID。
    attr.ah_attr.is_global = 1;              // 1 表示启用 GRH（全局路由头）
    attr.ah_attr.port_num = port_num;        // 出口端口
    attr.ah_attr.grh.hop_limit = 64;         // 类似 IP TTL，限制转发跳数
    attr.ah_attr.grh.traffic_class = 0;      // QoS 类别，示例置 0
    attr.ah_attr.grh.flow_label = 0;         // 流标签，示例置 0
    attr.ah_attr.grh.sgid_index = gid_index; // 源 GID 索引（本机）
    attr.ah_attr.grh.dgid = local_gid;       // 目标 GID（Loopback 配本机）

    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(qp, &attr, flags))
    {
        fprintf(stderr, "将 QP 切换到 RTR 失败\n");
        return 1;
    }

    // 3) RTR -> RTS (Ready to Send)
    // 进入 RTR 后，说明“收”已经可用，这时再打开“发”能力进入 RTS。
    // 可以理解为：先把 RQ 及路径准备好，再激活 SQ 发送。
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS; // 目标状态：RTS
    attr.timeout = 14;           // ACK 超时参数（指数编码）
    attr.retry_cnt = 7;          // 普通重试次数
    attr.rnr_retry = 7;          // RNR 重试次数（7 常表示“无限重试”）
    attr.sq_psn = 0;             // 发送方向起始 PSN
    attr.max_rd_atomic = 1;      // 本端可并发发起 RDMA Read/Atomic 的请求数

    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &attr, flags))
    {
        fprintf(stderr, "将 QP 切换到 RTS 失败\n");
        return 1;
    }

    return 0;
}

int main()
{
    struct ibv_device **dev_list;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_sge recv_sge;
    struct ibv_sge send_sge;
    struct ibv_recv_wr recv_wr;
    struct ibv_recv_wr *bad_recv_wr;
    struct ibv_send_wr send_wr;
    struct ibv_send_wr *bad_send_wr;
    struct ibv_wc wc[2];
    char *buf;
    char *send_buf;
    char *recv_buf;
    const char *msg = "hello rdma loopback";
    size_t msg_len = strlen(msg) + 1;
    const size_t buf_size = 4096;
    int completed = 0;
    int poll_round = 0;

#define CLEANUP_AND_RETURN()                                        \
    {                                                               \
        cleanup_rdma_resources(dev_list, ctx, pd, cq, qp, mr, buf); \
        return 1;                                                   \
    }

#define FAIL_AND_RETURN(err_msg)          \
    {                                     \
        fprintf(stderr, "%s\n", err_msg); \
        CLEANUP_AND_RETURN();             \
    }

    dev_list = NULL;
    ctx = NULL;
    pd = NULL;
    cq = NULL;
    qp = NULL;
    mr = NULL;
    buf = NULL;
    send_buf = NULL;
    recv_buf = NULL;

    // 1. 找网卡 (Get Device List)
    // 这一步之后，驱动程序会在内核里为你这个进程创建一个 Context。
    // 通过这个 Context，你的程序获得了直接访问网卡硬件寄存器的“通行证”。
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list)
    {
        perror("获取 IB 设备列表失败");
        return 1;
    }
    if (!dev_list[0])
    {
        fprintf(stderr, "未找到可用 IB 设备\n");
        CLEANUP_AND_RETURN();
    }

    // 2. 开网卡 (Open Device) - 假设用列表里的第一个
    ctx = ibv_open_device(dev_list[0]);
    if (!ctx)
    {
        fprintf(stderr, "打开设备失败: %s\n", ibv_get_device_name(dev_list[0]));
        CLEANUP_AND_RETURN();
    }
    printf("成功打开网卡: %s\n", ibv_get_device_name(dev_list[0]));

    // 3. 分配 PD (Alloc PD) - 你的隔离房间
    // 你在网卡硬件里划出了一块“私有领地”。以后你注册的内存（MR）和创建的 QP 只要填入这个 PD 句柄，
    // 硬件就会自动识别它们属于同一个保护域。
    pd = ibv_alloc_pd(ctx);
    if (!pd)
    {
        FAIL_AND_RETURN("分配 PD 失败");
    }

    // 4. 创建 CQ (Create CQ) - 你的收件箱，深度设为 100
    cq = ibv_create_cq(ctx, 100, NULL, NULL, 0);
    if (!cq)
    {
        FAIL_AND_RETURN("创建 CQ 失败");
    }

    // 5. 创建 QP (Create QP) - 雇佣搬运工
    // 当你调用 create_qp 时，网卡会真的在自己的片上内存
    // 或者 host 内存里开辟一段空间来管理这个 QP 的状态。
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC; // 可靠连接类型
    qp_init_attr.cap.max_send_wr = 10; // 发送队列深度
    qp_init_attr.cap.max_recv_wr = 10; // 接收队列深度
    qp_init_attr.cap.max_send_sge = 1; // 每次搬运允许碎片的个数
    qp_init_attr.cap.max_recv_sge = 1; // 每个接收 WR 最多 1 个数据片段

    qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp)
    {
        FAIL_AND_RETURN("创建 QP 失败");
    }

    printf("恭喜！所有 RDMA 静态资源已就绪。\n");
    printf("QP 编号: 0x%x\n", qp->qp_num);

    // 把 QP 推进到 RTS，真正“激活”这条 RC 连接。
    // 你的实验目标是 Loopback，所以这里会把目的 QPN/GID 配成自己。
    if (modify_qp_to_rts(qp))
    {
        FAIL_AND_RETURN("将 QP 激活到 RTS 失败");
    }
    printf("QP 状态已切换到 RTS（Loopback 配置）\n");

    // 6. 准备一块用户态内存并注册为 MR（Memory Region）
    // RDMA 网卡不会直接信任任意虚拟地址。
    // 只有先把内存注册成 MR，网卡才会记录这段地址的访问权限和地址翻译信息，
    // 后续 SGE 里带上 lkey 后，硬件才能安全地 DMA 读/写这块内存。
    buf = calloc(1, buf_size);
    if (!buf)
    {
        FAIL_AND_RETURN("分配缓冲区失败");
    }

    // 一个 MR 里切两段：前半段当发送缓冲区，后半段当接收缓冲区。
    // 这么做的目的：示例简单、内存连续、只需要注册一次 MR。
    // 发送和接收都在同一块注册内存里，便于观察 loopback 前后数据变化。
    send_buf = buf;
    recv_buf = buf + 2048;
    if (msg_len > 2048)
    {
        FAIL_AND_RETURN("消息过大，超过示例缓冲区分段大小");
    }
    memcpy(send_buf, msg, msg_len);

    // 注册 MR，拿到 lkey/rkey。
    // - lkey: 本地网卡访问这块内存时用于校验权限（本例 post_send/post_recv 必须用）。
    // - rkey: 远端执行 RDMA Read/Write 访问这块内存时使用（本例 loopback SEND 不直接用到）。
    mr = ibv_reg_mr(pd, buf, buf_size,
                    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!mr)
    {
        FAIL_AND_RETURN("注册 MR 失败");
    }

    // 7. 先 post 一个接收 WR 到 RQ（非常关键）
    // RC 模式下如果对端先发而你还没挂 recv，接收队列没“空位”就会触发 RNR（Receiver Not Ready）。
    // 所以最佳实践是：先挂 recv，再发 send。
    memset(&recv_sge, 0, sizeof(recv_sge));
    recv_sge.addr = (uintptr_t)recv_buf; // addr: 目标数据缓冲区起始地址
    recv_sge.length = msg_len;           // length: 允许写入的最大字节数
    recv_sge.lkey = mr->lkey;            // lkey: 告诉网卡这段地址属于哪个已注册 MR

    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id = 1;           // wr_id: 用户自定义标签，CQE 返回时可据此识别“这是哪条请求”
    recv_wr.sg_list = &recv_sge; // sg_list: 指向一个或多个数据片段描述符（SGE）
    recv_wr.num_sge = 1;         // num_sge: 本 WR 使用 1 个片段，覆盖 recv_buf 这一段

    if (ibv_post_recv(qp, &recv_wr, &bad_recv_wr))
    {
        FAIL_AND_RETURN("投递 RECV WR 失败");
    }

    // 8. 再 post 一个 SEND WR 到 SQ，发往“自己”
    // Loopback 场景中，发送路径和接收路径都在本机闭环，
    // 但流程和真实双机场景一致：SQ 发，RQ 收，CQ 回报完成。
    memset(&send_sge, 0, sizeof(send_sge));
    send_sge.addr = (uintptr_t)send_buf; // 从 send_buf 这段内存读取待发送数据
    send_sge.length = msg_len;           // 发送字节数
    send_sge.lkey = mr->lkey;            // 对发送源地址做本地访问校验

    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id = 2;                      // 发送 WR 的标签，和 recv_wr.wr_id 用不同值便于区分
    send_wr.sg_list = &send_sge;            // 指向发送数据片段描述
    send_wr.num_sge = 1;                    // 单片段发送
    send_wr.opcode = IBV_WR_SEND;           // 普通 SEND：把数据投递到对端 RQ（不是 RDMA Write）
    send_wr.send_flags = IBV_SEND_SIGNALED; // 产生 SEND CQE；教学场景建议每次都信号化，便于观察

    if (ibv_post_send(qp, &send_wr, &bad_send_wr))
    {
        FAIL_AND_RETURN("投递 SEND WR 失败");
    }

    // 9. 轮询 CQ，等待 2 个完成事件（一个 RECV，一个 SEND）
    // ibv_poll_cq 的返回值 n 表示“这次取到了几个 CQE”。
    // 这里目标是收齐 2 个 CQE，证明收发两条 WR 都已完成。
    // 这是最直观的教学写法：忙轮询。实际工程可用事件通知降低 CPU 占用。
    while (completed < 2)
    {
        int n;
        int i;

        // 从 CQ 中最多取 (2 - completed) 个事件，直接追加写入 wc[] 的未用区域。
        n = ibv_poll_cq(cq, 2 - completed, &wc[completed]);
        if (n < 0)
        {
            FAIL_AND_RETURN("ibv_poll_cq 调用失败");
        }

        completed += n; // 累加当前已完成的 CQE 数量
        poll_round++;
        if (poll_round > 1000000)
        {
            fprintf(stderr, "等待 CQE 超时，当前完成数 %d/2\n", completed);
            CLEANUP_AND_RETURN();
        }

        for (i = 0; i < completed; i++)
        {
            // 每个 CQE 都必须检查 status。
            // IBV_WC_SUCCESS 表示该 WR 被硬件成功执行。
            if (wc[i].status != IBV_WC_SUCCESS)
            {
                fprintf(stderr, "CQE[%d] 异常, status=%d, wr_id=%llu\n",
                        i, wc[i].status, (unsigned long long)wc[i].wr_id);
                CLEANUP_AND_RETURN();
            }
        }
    }

    printf("Loopback 的 CQ 完成事件数量: %d\n", completed);
    printf("接收缓冲区内容: %s\n", recv_buf);

    cleanup_rdma_resources(dev_list, ctx, pd, cq, qp, mr, buf);

#undef FAIL_AND_RETURN
#undef CLEANUP_AND_RETURN

    return 0;
}