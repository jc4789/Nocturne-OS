import { getB } from './cycle-b.mjs';
export function getA() { return 'A'; }
export function getCycle() { return getA() + getB(); }
