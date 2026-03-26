from pyverbs.device import Context
from pyverbs.pd import PD
from pyverbs.mr import MR
import pyverbs.enums as e

ctx = Context(name='mlx5_0')

# 创建两个独立的房间（PD）
pd_secure = PD(ctx)
pd_other = PD(ctx)

# 在 pd_secure 房间里注册一个 MR
mr = MR(pd_secure, 1024, e.IBV_ACCESS_LOCAL_WRITE)

print(f"MR 属于 PD 句柄: {pd_secure.handle}")

# 思考题：如果我们后面创建一个属于 pd_other 的 QP，
# 并强行让它使用这个 MR 的 lkey，网卡会发生什么？
# 结果：硬件会直接在下发任务时报错，因为 PD 校验不通过。
