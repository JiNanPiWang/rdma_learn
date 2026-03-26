from pyverbs.device import Context
from pyverbs.pd import PD
from pyverbs.mr import MR
import pyverbs.enums as e

# 1. 打开网卡 (对应你的 mlx5_0)
ctx = Context(name='mlx5_0')

# 2. 创建保护域 (Protection Domain, 第 7 节内容)
# 把它想象成一个资源隔离的“容器”
pd = PD(ctx)

# 3. 准备一段普通内存 (比如 1024 字节)
resource_size = 1024

# 4. 注册 MR (Memory Region, 第 6 节核心)
# access 参数定义了权限：本地读写 + 远程读写
mr = MR(pd, resource_size, 
        e.IBV_ACCESS_LOCAL_WRITE | 
        e.IBV_ACCESS_REMOTE_WRITE | 
        e.IBV_ACCESS_REMOTE_READ)

# --- 实验观察点 ---
print(f"MR 注册成功！")
# 在 Pyverbs 中，地址属性通常叫 addr
# print(f"虚拟地址 (Addr): {hex(mr.addr)}")
print(f"本地钥匙 (L_Key): {hex(mr.lkey)}")
print(f"远程钥匙 (R_Key): {hex(mr.rkey)}")
print(f"内存长度 (Length): {mr.length} 字节")