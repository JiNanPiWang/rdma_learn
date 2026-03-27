#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h> // 必须包含这个头文件

int main() {
    struct ibv_device **dev_list;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_qp_init_attr qp_init_attr;

    // 1. 找网卡 (Get Device List)
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("Failed to get IB devices list");
        return 1;
    }

    // 2. 开网卡 (Open Device) - 假设用列表里的第一个
    ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "Couldn't open device %s\n", ibv_get_device_name(dev_list[0]));
        return 1;
    }
    printf("成功打开网卡: %s\n", ibv_get_device_name(dev_list[0]));

    // 3. 分配 PD (Alloc PD) - 你的隔离房间
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        return 1;
    }

    // 4. 创建 CQ (Create CQ) - 你的收件箱，深度设为 100
    cq = ibv_create_cq(ctx, 100, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "Couldn't create CQ\n");
        return 1;
    }

    // 5. 创建 QP (Create QP) - 雇佣搬运工
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC; // 可靠连接类型
    qp_init_attr.cap.max_send_wr = 10;  // 发送队列深度
    qp_init_attr.cap.max_recv_wr = 10;  // 接收队列深度
    qp_init_attr.cap.max_send_sge = 1; // 每次搬运允许碎片的个数

    qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) {
        fprintf(stderr, "Couldn't create QP\n");
        return 1;
    }

    printf("恭喜！所有 RDMA 静态资源已就绪。\n");
    printf("QP Number: 0x%x\n", qp->qp_num);

    // 记得打扫战场
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);

    return 0;
}