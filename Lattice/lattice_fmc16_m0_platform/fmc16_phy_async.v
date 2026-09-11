`timescale 1ns/1ps
//=============================================================================
// fmc16_phy_async.v
//------------------------------------------------------------------------------
// FMC16 异步从接口物理层（LCMXO2-4000HC FMC16 personality）
//
// 职责（契约 fmc16_design_contract.md §3.3、§4，实施计划 §6）：
//   - NE1/NOE/NWE 各两级同步进 50MHz 系统域；周期状态（IDLE/READ/WRITE/
//     INACTIVE/ILLEGAL）由同步后信号组合译码并每周期寄存；
//   - 写方向：raw legal-write 窗口（!NE1 && NOE && !NWE，raw 引脚）内逐拍
//     采样 DB（db_last/db_prev 链）并计数 wr_cycles；WRITE→IDLE 边沿按
//     WRITE_MIN_CYCLES 提交 rx_word_valid/rx_word=db_last（raw 窗口内
//     最后一次采样），否则 rx_abort；wr_cycles 离开 WRITE 即清零；
//   - 读方向：IDLE 且未持有时预取 TX store 当前指针 word 到 tx_hold；
//     READ→IDLE 边沿若持有有效 word 则单拍 tx_advance 并清持有；非法/换向
//     边沿不推进、不清持有，重试读得到同一 word；空 TX 读返回 0xFFFF 且
//     不推进；
//   - 总线驱动：仅 READ 状态组合驱动（基于寄存 state，NOE 释放后 20ns 内
//     高阻），非法周期、INACTIVE（NE1=1）、写周期一律高阻；
//   - rx_abort 触发边沿：进入 ILLEGAL（仅一次）、READ↔WRITE 直接换向、
//     写周期过短/采样不稳定；进入 INACTIVE 不触发（毛刺容忍）；
//   - 非法周期计数只留内部寄存器，不进入任何公开端口（契约 §4.1/§4.2）。
//
// 禁止事项（契约 §4）：无 debug/probe/test 公开端口；NE1/NOE/NWE 不作
// 全局时钟；不实例化 PLL/SPI；写周期只采样不驱动。
//
// 与 TX store（契约 §3.6）的时序约定：tx_advance 置位当拍，tx_word 必须
// 已反映推进后的下一个 word（TX store 读指针须组合推进或在同拍给出新
// word），否则 IDLE 预取会取到刚读过的同一 word 造成重复交付。
//=============================================================================

module fmc16_phy_async #(
    //---------------------------------------------------------------------------
    // WRITE_MIN_CYCLES：WRITE 状态内至少持续的 sys_clk 采样周期数，少于则
    // rx_abort。
    // 100ns 写合同依据（实施计划 §6）：STM32 侧 NWE 低电平最小 100ns，即
    // 50MHz 下 5 个周期；经两级同步与周期译码后，合法写周期在 WRITE 状态内
    // 可获得约 3~5 个采样周期。WRITE_MIN_CYCLES=2 仅用于滤除 <100ns 的 NWE
    // 毛刺与过短写（WRITE 状态持续时间不足的写周期直接放弃），对满足合同的
    // 正常写留足余量；提交还要求连续两次总线采样一致（db_s1==db_s2）。
    //---------------------------------------------------------------------------
    parameter WRITE_MIN_CYCLES = 2
)(
    input  wire        sys_clk,       // 50MHz 系统时钟
    input  wire        rst_n,         // 异步复位，低有效
    input  wire        fmc_ne1_n,     // FMC 片选（低有效）
    input  wire        fmc_noe_n,     // FMC 读选通（低有效）
    input  wire        fmc_nwe_n,     // FMC 写选通（低有效）
    inout  wire [15:0] fmc_db,        // FMC 数据总线（仅 READ 状态驱动）
    output reg         rx_word_valid, // 合法写周期完成，提交一个 word
    output reg  [15:0] rx_word,       // 提交的 word
    output reg         rx_abort,      // 非法周期/写不稳定/直接换向：单周期脉冲
    input  wire [15:0] tx_word,       // TX store 当前指针 word（空=0xFFFF）
    input  wire        tx_have_word,  // TX store 当前指针有有效 word
    output reg         tx_advance     // 完成合法读周期且交付有效 word：单脉冲
);

    //---------------------------------------------------------------------------
    // 周期状态编码
    //---------------------------------------------------------------------------
    localparam [2:0] ST_INACTIVE = 3'd0,   // NE1=1：未选中，无条件高阻
                     ST_IDLE     = 3'd1,   // NE1=0 且 NOE=NWE=1：选中无选通
                     ST_READ     = 3'd2,   // NE1=0, NOE=0, NWE=1：读周期
                     ST_WRITE    = 3'd3,   // NE1=0, NOE=1, NWE=0：写周期
                     ST_ILLEGAL  = 3'd4;   // NOE=0 且 NWE=0：非法周期

    //---------------------------------------------------------------------------
    // 控制输入两级同步（CDC：FMC 异步控制脚 → 50MHz 系统域）
    // 复位值取 1：NE1/NOE/NWE 均视为无效电平（高），避免上电瞬间误判非法周期
    //---------------------------------------------------------------------------
    reg ne1_s0, ne1_s1;
    reg noe_s0, noe_s1;
    reg nwe_s0, nwe_s1;

    always @(posedge sys_clk or negedge rst_n) begin
        if (!rst_n) begin
            ne1_s0 <= 1'b1; ne1_s1 <= 1'b1;
            noe_s0 <= 1'b1; noe_s1 <= 1'b1;
            nwe_s0 <= 1'b1; nwe_s1 <= 1'b1;
        end
        else begin
            ne1_s0 <= fmc_ne1_n; ne1_s1 <= ne1_s0;
            noe_s0 <= fmc_noe_n; noe_s1 <= noe_s0;
            nwe_s0 <= fmc_nwe_n; nwe_s1 <= nwe_s0;
        end
    end

    //---------------------------------------------------------------------------
    // 周期分类（同步后信号组合译码）与寄存的周期状态
    //---------------------------------------------------------------------------
    wire cyc_illegal = !ne1_s1 && !noe_s1 && !nwe_s1;   // NOE 与 NWE 同时为 0
    wire cyc_read    = !ne1_s1 && !noe_s1 &&  nwe_s1;   // 读：NE1=0, NOE=0, NWE=1
    wire cyc_write   = !ne1_s1 &&  noe_s1 && !nwe_s1;   // 写：NE1=0, NOE=1, NWE=0
    wire cyc_idle    = !ne1_s1 &&  noe_s1 &&  nwe_s1;   // 选中但无选通
    // 其余组合（NE1=1）→ INACTIVE

    wire [2:0] next_state =
          cyc_illegal ? ST_ILLEGAL  :
          cyc_read    ? ST_READ     :
          cyc_write   ? ST_WRITE    :
          cyc_idle    ? ST_IDLE     : ST_INACTIVE;

    reg [2:0] state;

    always @(posedge sys_clk or negedge rst_n) begin
        if (!rst_n) state <= ST_INACTIVE;
        else        state <= next_state;
    end

    //---------------------------------------------------------------------------
    // 读路径：raw 数据直通、同步周期结束后推进一次。
    // STM32 FMC 的 ADDSET 可能短于两个 50 MHz 周期，不能依赖 ST_IDLE
    // 预取，否则 INACTIVE→READ 会重复交付上一字。
    //---------------------------------------------------------------------------
    wire raw_read_active = !fmc_ne1_n && !fmc_noe_n && fmc_nwe_n;
    reg read_in_progress;
    reg read_had_word;

    always @(posedge sys_clk or negedge rst_n) begin
        if (!rst_n) begin
            read_in_progress <= 1'b0;
            read_had_word    <= 1'b0;
        end
        else if (cyc_read) begin
            if (!read_in_progress) begin
                read_in_progress <= 1'b1;
                read_had_word    <= raw_read_active && tx_have_word;
            end
            else if (raw_read_active && tx_have_word) begin
                read_had_word <= 1'b1;
            end
        end
        else if (read_in_progress) begin
            read_in_progress <= 1'b0;
            read_had_word    <= 1'b0;
        end
    end

    //---------------------------------------------------------------------------
    // 写路径（2026-08-13 审查三轮 P0 定案）：采样与周期结束判定解耦，
    // 提交 raw legal-write 窗口内的【最后一次采样】。
    //   - raw legal-write gate（契约 write_active = !NE1 && NOE && !NWE，raw
    //     引脚）：窗口内逐拍 db_last<=fmc_db、db_prev<=db_last、wr_cycles++；
    //   - 同步状态机晚 40~60ns 才确认写结束（WRITE→IDLE/INACTIVE），提交
    //     db_last（raw 窗口内最后一次采样），不再采已三态的 DB；
    //   - 非法组合（NOE=NWE=0）时 raw gate 关闭，raw capture 不采样（与
    //     契约 write_active 定义一致，审查三轮要求）。
    // 正确性论证（针对合同最坏相位：最终数据恰在 NWE 上升前 40ns 才稳定，
    // NWE 低电平恰 100ns）：
    //   设最终数据在 T_change 稳定，NWE 上升于 T_rise，合同给
    //   T_rise - T_change >= 40ns；50MHz 采样周期 20ns，raw 窗口内最后一次
    //   采样沿 E_last 满足 T_rise - 20ns < E_last <= T_rise（E_last 最坏与
    //   NWE 上升同沿竞争，采样点在 hold 窗口内、DB 仍被主端驱动）。于是
    //   E_last - T_change >= 20ns > 0：最后一次采样必然落在最终数据稳定
    //   之后 → 提交 db_last 恒为最终值。
    // 旧实现（db_s1==db_s2 一致才更新 candidate）多等一拍确认：最坏相位下
    // 采样链在 T_rise 前已成为 B/B，但 candidate 仍停在旧值 A，ev_write_end
    // 提交 A（确定性错误，TB 相位扫描 T22/T23 复现并回归）。一致判定不能
    // 作为提交门禁——它恰好会拒绝合法的“单次干净采样”最坏相位写；
    // 正确做法是直接提交 raw 窗口内最后一次采样，把“稳定判定”交给合同
    // 的 40ns setup 保证。
    //---------------------------------------------------------------------------
    wire raw_write_active = !fmc_ne1_n && fmc_noe_n && !fmc_nwe_n;

    reg [15:0] db_prev;            // 上一拍采样（保留，供调试/扩展，不进提交）
    reg [15:0] db_last;            // raw 窗口内最后一次采样（提交值）
    reg [7:0]  wr_cycles;          // raw 窗口内采样周期计数

    always @(posedge sys_clk or negedge rst_n) begin
        if (!rst_n) begin
            db_prev   <= 16'd0;
            db_last   <= 16'd0;
            wr_cycles <= 8'd0;
        end
        else if (raw_write_active) begin
            // raw legal-write 窗口：逐拍采样（提交用 db_last）
            db_prev   <= db_last;
            db_last   <= fmc_db;
            wr_cycles <= wr_cycles + 8'd1;
        end
        else if (state == ST_WRITE) begin
            // NWE 已释放但同步状态仍 WRITE（延迟 40~60ns）：不再采样，
            // 保持 db_last 与 wr_cycles，等待同步结束判定提交。
        end
        else begin
            // 非写周期：清采样状态
            db_prev   <= 16'd0;
            db_last   <= 16'd0;
            wr_cycles <= 8'd0;
        end
    end

    //---------------------------------------------------------------------------
    // 事件译码（组合，基于寄存 state 与 next_state 的边沿条件）
    //---------------------------------------------------------------------------
    // 进入 ILLEGAL：仅一次（停留在 ILLEGAL 内不再触发）
    wire ev_enter_illegal = (state != ST_ILLEGAL) && (next_state == ST_ILLEGAL);
    // READ↔WRITE 直接换向（未经过 IDLE）
    wire ev_switch_rd_wr  = (state == ST_READ  && next_state == ST_WRITE) ||
                            (state == ST_WRITE && next_state == ST_READ);
    // 读周期正常结束（READ→IDLE 或 READ→INACTIVE）
    // 修正记录：STM32 结束读访问时若同时释放 NOE 与 NE1（异步时序常见），
    // 同步后状态为 READ→INACTIVE，原实现只认 READ→IDLE 会静默漏 advance。
    // NE1 释放（进入 INACTIVE）即合法周期结束；直接换向（READ→WRITE）仍按
    // 非法处理（ev_switch_rd_wr）。
    wire ev_read_end      = (state == ST_READ) &&
                            ((next_state == ST_IDLE) || (next_state == ST_INACTIVE));
    // 写周期结束（WRITE→IDLE 或 WRITE→INACTIVE，同上理）
    wire ev_write_end     = (state == ST_WRITE) &&
                            ((next_state == ST_IDLE) || (next_state == ST_INACTIVE));
    // 采样可判定性：总线悬空（z）时采样为 x。对真实 0/1 数据本判断恒为真
    // （硬件中无 x），仅用于仿真确定性：未知采样一律按“不稳定”放弃，
    // 避免 x 进入 rx_word/rx_abort 污染下游。
    wire db_samples_known = (^db_last === 1'b0) || (^db_last === 1'b1);
    // 提交条件：采样周期足够 且 raw 窗口内最后一次采样可判定。
    // 提交 db_last = 最坏相位（40ns setup）下仍恒为最终值（见上论证）。
    wire ev_write_ok      = ev_write_end &&
                            (wr_cycles >= WRITE_MIN_CYCLES) &&
                            db_samples_known;
    // 放弃条件：过短 / 采样未知
    wire ev_write_bad     = ev_write_end && !ev_write_ok;

    //---------------------------------------------------------------------------
    // 输出脉冲（全部单周期）：
    //   rx_word_valid / rx_word —— 合法写周期提交
    //   rx_abort                —— 非法周期/换向/写不稳定
    //   tx_advance              —— 合法读周期交付有效 word
    //---------------------------------------------------------------------------
    always @(posedge sys_clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_word_valid <= 1'b0;
            rx_word       <= 16'd0;
            rx_abort      <= 1'b0;
            tx_advance    <= 1'b0;
        end
        else begin
            if (ev_write_ok) begin
                rx_word_valid <= 1'b1;        // 提交一个 word
                rx_word       <= db_last;     // 提交 raw 窗口内最后一次采样
            end
            else begin
                rx_word_valid <= 1'b0;
            end
            // 非法边沿：进入 ILLEGAL / READ↔WRITE 直接换向 / 写周期过短或不稳定
            rx_abort <= ev_enter_illegal || ev_switch_rd_wr || ev_write_bad;
            // 仅在 READ 正常结束到 IDLE/INACTIVE 时推进；直接换向/非法周期
            // 不推进，因而重试仍返回同一字。
            tx_advance <= ev_read_end && read_had_word;
        end
    end

    //---------------------------------------------------------------------------
    // 非法周期计数：仅内部统计（契约 §3.3/§4.2），不进入任何公开端口
    //---------------------------------------------------------------------------
    reg [31:0] illegal_cycle_count;

    always @(posedge sys_clk or negedge rst_n) begin
        if (!rst_n) illegal_cycle_count <= 32'd0;
        else if (ev_enter_illegal) illegal_cycle_count <= illegal_cycle_count + 32'd1;
    end

    //---------------------------------------------------------------------------
    // 总线驱动按 raw 合法读窗立即开关。短 ADDSET 下无需等待同步 state，
    // 写、未选中及非法周期仍始终高阻；空 TX 返回 0xFFFF 且不推进。
    //---------------------------------------------------------------------------
    assign fmc_db = rst_n && raw_read_active ?
                    (tx_have_word ? tx_word : 16'hFFFF) : 16'hzzzz;

endmodule
