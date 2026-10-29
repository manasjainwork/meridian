export type Level = [number, number]; // [price, qty]
export interface Frame { t: number; b: Level[]; a: Level[]; }
export interface Trade { t: number; s: "buy" | "sell"; p: number; q: number; }
export interface Replay { symbol: string; depth: number; source: string; frames: Frame[]; trades?: Trade[]; gaps_capped_ms?: number; sampled_ms?: number; }
