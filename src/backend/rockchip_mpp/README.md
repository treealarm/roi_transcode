# Rockchip MPP backend — not implemented

TZ v2.0 §6 makes it P0 for RK3588/RK3588S. Deferred: no board.

§6.4 is the reason this cannot be stubbed sensibly — the number of ROI regions and the QP
API differ between SoCs, so the backend is defined by what runtime capability query
returns, and that answer only exists on hardware.
