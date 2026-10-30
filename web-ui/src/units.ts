// Prices are integer ticks and quantities integer lots in the gateway. These helpers convert to and from decimal text
// without floating point, so what the user types is exactly what is sent.
export function parseUnits(text: string, decimals: number): number | null {
  const m = /^(\d+)(?:\.(\d*))?$/.exec(text.trim());
  if (!m) return null;
  const frac = m[2] ?? "";
  if (frac.length > decimals) return null;                       // more decimals than a tick or lot allows: refuse, do not round
  const n = Number(m[1] + frac.padEnd(decimals, "0"));
  return Number.isSafeInteger(n) ? n : null;
}

export function fmtUnits(n: number, decimals: number, group = true): string {
  const neg = n < 0, a = Math.abs(n), p = 10 ** decimals;
  const int = Math.floor(a / p), frac = String(a % p).padStart(decimals, "0");
  const ints = group ? int.toLocaleString("en-US") : String(int);
  return (neg ? "-" : "") + ints + (decimals ? "." + frac : "");
}
