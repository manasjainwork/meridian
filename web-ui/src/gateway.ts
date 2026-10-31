export interface Config { price_decimals: number; qty_decimals: number; max_price_ticks: number; max_order_qty_lots: number; max_position_lots: number; }
export interface Fill { maker: number; taker: number; price: number; qty: number; }
export interface OrderResult { ok: boolean; reject: string; order_id: number; status: string; filled: number; rested: number; cancelled: number; duplicate: boolean; triggered_fills?: number; fills: Fill[]; error?: string; }
export interface BookData { bids: [number, number][]; asks: [number, number][]; }
export interface TradeItem { seq: number; price: number; qty: number; aggressor: "buy" | "sell"; }
export interface OpenOrder { order_id: number; cl_ord_id: number; side: "buy" | "sell"; price: number; qty: number; filled: number; remaining: number; }
export interface StopOrder { order_id: number; side: "buy" | "sell"; stop_price: number; price: number; qty: number; }
export interface AccountData { account: number; position: number; cash: number; open_buy: number; open_sell: number; mark: number | null; equity: number | null; open_orders: OpenOrder[]; stops: StopOrder[]; }

export async function api<T>(base: string, method: "GET" | "POST", path: string, body?: unknown): Promise<{ status: number; data: T }> {
  const r = await fetch(base + path, { method, headers: body ? { "Content-Type": "application/json" } : undefined, body: body ? JSON.stringify(body) : undefined });
  return { status: r.status, data: (await r.json()) as T };
}
