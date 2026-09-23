"use strict";

const ObserverMemory = (() => {
  const labels = {
    WORLD_EVENT: "Událost ve světě", PLAYER_SEEN: "Spatřený hráč", CREATURE_SEEN: "Spatřená bytost",
    CREATURE_KILLED: "Zabití bytosti", NPC_INJURED: "Zranění NPC", ITEM_STOLEN: "Krádež předmětu",
    TRADE_COMPLETED: "Dokončený obchod", LIVESTOCK_KILLED: "Zabití hospodářského zvířete",
    WOLF_PACK_MOVED: "Přesun vlčí smečky", FOOD_SHORTAGE: "Nedostatek jídla", NPC_DIED: "Úmrtí NPC",
    DYNAMIC_QUEST_COMPLETED: "Dokončený úkol", DYNAMIC_QUEST_FAILED: "Neúspěšný úkol",
    DYNAMIC_QUEST_EXPIRED: "Vypršelý úkol", SIGHT: "Viděl", HEARING: "Slyšel", RUMOR: "Doslechl se",
  };
  function label(value) { return labels[value] || value || "—"; }
  function date(value) { return value ? new Date(value).toLocaleString("cs-CZ") : "—"; }
  function entity(value) {
    if (!value || value.kind === "UNKNOWN") return "—";
    if (value.kind === "PLAYER") return "Hráč";
    const ids = [];
    if (value.agent_id !== "0") ids.push(`Agent ${value.agent_id}`);
    if (value.spawn_id !== "0") ids.push(`Spawn ${value.spawn_id}`);
    if (value.entry) ids.push(`Entry ${value.entry}`);
    return [value.name || "NPC", ...ids].join(" · ");
  }
  function bounds(page) {
    return { previous: page.offset > 0, next: page.offset + page.records.length < page.anchor,
      summary: page.records.length ? `${page.offset + 1}–${page.offset + page.records.length} z ${page.anchor}` :
        page.anchor ? `Na této stránce nejsou záznamy (celkem ${page.anchor}).` : "Žádné dlouhodobé vzpomínky." };
  }
  return { label, date, entity, bounds };
})();

// Independent of map polling: a response for an earlier selection can never
// replace the selected NPC's memory. Only the visible page is requested.
class ObserverMemoryPanel {
  constructor() {
    this.agentId = null;
    this.version = 0;
    this.generation = 0;
    this.offset = 0;
    this.anchor = 0;
    this.pageSize = 25;
    this.timer = null;
    this.controller = null;
    this.status = document.getElementById("memory-status");
    this.list = document.getElementById("memory-list");
    this.previous = document.getElementById("memory-prev");
    this.next = document.getElementById("memory-next");
    this.refresh = document.getElementById("memory-refresh");
    this.previous.addEventListener("click", () => this.load(Math.max(0, this.offset - this.pageSize), this.anchor));
    this.next.addEventListener("click", () => this.load(this.offset + this.pageSize, this.anchor));
    this.refresh.addEventListener("click", () => this.load(0, 0, true));
  }

  select(agent, version) {
    const id = agent?.agent_id ?? null;
    if (id === this.agentId && version === this.version) return;
    this.agentId = id;
    this.version = version;
    this.load(0, 0);
  }

  load(offset, anchor, refresh = false) {
    clearTimeout(this.timer);
    this.controller?.abort();
    this.controller = new AbortController();
    const generation = ++this.generation;
    this.offset = offset;
    this.anchor = anchor;
    this.list.replaceChildren();
    this.previous.disabled = this.next.disabled = true;
    this.refresh.disabled = this.agentId == null || this.version < 3;
    if (this.agentId == null) { this.status.textContent = ""; return; }
    if (this.version < 3) {
      this.status.textContent = "Pro zobrazení paměti aktualizujte worldserver.";
      return;
    }
    this.status.textContent = "Načítám dlouhodobou paměť…";
    const url = `/api/agents/${encodeURIComponent(this.agentId)}/memory?offset=${offset}&anchor=${anchor}`;
    const signal = this.controller.signal;
    const started = performance.now();
    const request = async (fresh) => {
      try {
        const response = await fetch(`${url}${fresh ? "&refresh=true" : ""}`, { cache: "no-store", signal });
        if (generation !== this.generation) return;
        if (response.status === 429) throw new Error("Paměť právě načítá více uživatelů. Zkuste ji za chvíli obnovit.");
        if (!response.ok) throw new Error(response.status === 404 ? "Agent už není v aktuálním snímku." : "Paměť se nepodařilo načíst.");
        const page = await response.json();
        if (generation !== this.generation) return;
        if (page.status === "pending" && performance.now() - started < 45000) {
          this.timer = setTimeout(() => request(false), 1000);
        } else if (page.status === "ready") {
          this.render(page);
        } else {
          this.status.textContent = page.status === "unsupported" ? "Pro zobrazení paměti aktualizujte worldserver." :
            page.status === "offline" ? "Worldserver právě neposílá aktuální data. Zkuste paměť obnovit po připojení." :
              "Worldserver zatím nevrátil paměť. Zkuste ji obnovit.";
        }
      } catch (error) {
        if (generation === this.generation && error.name !== "AbortError")
          this.status.textContent = error.message || "Paměť se nepodařilo načíst.";
      }
    };
    request(refresh);
  }

  render(page) {
    this.anchor = page.anchor;
    this.pageSize = page.page_size;
    const bounds = ObserverMemory.bounds(page);
    this.previous.disabled = !bounds.previous;
    this.next.disabled = !bounds.next;
    this.status.textContent = `${bounds.summary} · Zachyceno ${ObserverMemory.date(page.captured_at_ms)}` +
      (page.total > page.anchor ? ` · ${page.total - page.anchor} nových — zobrazíte obnovením` : "");
    this.list.replaceChildren(...page.records.map(memory => {
      const card = document.createElement("article"); card.className = "memory-card";
      const title = document.createElement("h3"); title.textContent = ObserverMemory.label(memory.event_type || memory.type);
      const meta = document.createElement("p");
      meta.textContent = `Důležitost ${Math.round(memory.importance * 100)} % · ${ObserverMemory.label(memory.channel)} · ${memory.observation_count}×`;
      const facts = document.createElement("dl"); facts.className = "facts";
      const rows = [["Poprvé", ObserverMemory.date(memory.first_observed_at_ms)],
        ["Naposledy", ObserverMemory.date(memory.last_observed_at_ms)],
        ["Aktér", ObserverMemory.entity(memory.actor)], ["Cíl", ObserverMemory.entity(memory.target)],
        ["Místo", `Map ${memory.location.map_id} · ${memory.location.x.toFixed(1)}, ${memory.location.y.toFixed(1)}, ${memory.location.z.toFixed(1)}`]];
      for (const [name, value] of rows) {
        const row = document.createElement("div"), dt = document.createElement("dt"), dd = document.createElement("dd");
        dt.textContent = name; dd.textContent = value; row.append(dt, dd); facts.append(row);
      }
      const details = document.createElement("details"), summary = document.createElement("summary"), raw = document.createElement("p");
      summary.textContent = "Identifikace události";
      raw.textContent = `${memory.type} / ${memory.event_type || "—"} · Událost ${memory.source_event_id} · Korelace ${memory.correlation_id}` +
        ` · Nastala ${ObserverMemory.date(memory.source_occurred_at_ms)}` +
        (memory.persistent_id === "0" ? " · DB ID není v běžící paměti známé" : ` · DB ID ${memory.persistent_id}`);
      details.append(summary, raw); card.append(title, meta, facts, details); return card;
    }));
  }
}

if (typeof module !== "undefined") module.exports = { ObserverMemory, ObserverMemoryPanel };
