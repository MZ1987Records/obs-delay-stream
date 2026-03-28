export function getOptionalElement<T extends HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

export function getRequiredElement<T extends HTMLElement>(id: string): T {
  const el = getOptionalElement<T>(id);
  if (!el) {
    throw new Error(`Missing required element: ${id}`);
  }
  return el;
}

export function getRequiredInputElement(id: string): HTMLInputElement {
  return getRequiredElement<HTMLInputElement>(id);
}

export function getRequiredSelectElement(id: string): HTMLSelectElement {
  return getRequiredElement<HTMLSelectElement>(id);
}

export function getRequiredButtonElement(id: string): HTMLButtonElement {
  return getRequiredElement<HTMLButtonElement>(id);
}
