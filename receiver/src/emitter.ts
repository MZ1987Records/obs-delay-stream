type Listener = (data: never) => void;

export class Emitter<M extends Record<string, unknown>> {
  private map = new Map<keyof M, Set<Listener>>();

  on<K extends keyof M>(event: K, fn: (data: M[K]) => void): () => void {
    let s = this.map.get(event);
    if (!s) {
      s = new Set();
      this.map.set(event, s);
    }
    s.add(fn as Listener);
    return () => {
      s!.delete(fn as Listener);
    };
  }

  emit<K extends keyof M>(event: K, data: M[K]): void {
    this.map.get(event)?.forEach((fn) => (fn as (d: M[K]) => void)(data));
  }
}
