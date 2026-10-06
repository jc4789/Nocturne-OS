import { getA } from './cycle-a.mjs';
export function getB() { return getA() === 'A' ? 'B' : '?'; }
