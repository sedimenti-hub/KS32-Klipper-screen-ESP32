# Riassunto Dettagliato dello Sviluppo KlipperScreen_ESP32

Questo documento riassume le modifiche, le implementazioni e le correzioni effettuate durante le sessioni di sviluppo per l'interfaccia LVGL su ESP32-C3.

## 🛠 Hardware e Ambiente
- **Scheda:** ESP32-2424S012 (ESP32-C3)
- **Display:** GC9A01 240x240 circolare IPS TFT
- **Touch:** CST816S
- **Framework UI:** LVGL v8.4.0
- **Ambiente di Sviluppo:** PlatformIO

## 🚀 Funzionalità e UI Implementate

### 1. Ristrutturazione Pagina Principale (Dashboard)
- **Rimozione Grafico:** Il grafico centrale nella prima pagina è stato eliminato per fare spazio a nuovi dati.
- **Nuovo Layout Telemetria:** Al posto del grafico, è stato creato un contenitore a "semiluna" che segue la curvatura del display in alto.
- **Dati Inseriti:** All'interno del contenitore sono stati inseriti 4 riquadri per: Velocità (Speed), Accelerazione (Accel), Ventola (Fan) e Temperatura Host (Host).
- **Stile Dati:** Le unità di misura per Velocità e Accelerazione sono state rimosse. I valori di Velocità e Accelerazione sono stati allineati a destra all'interno delle loro celle.
- **Traduzione:** L'intera interfaccia è stata tradotta in inglese.
- **Anello di Stato:** Lo spessore dell'anello di stato esterno (status ring) è stato ridotto del 20%.

### 2. Interattività
- **Touch Rotation:** Corretto il comportamento del touch per seguire la rotazione del display impostata (90°).
- **Tasti Cliccabili:** Le celle di Velocità, Accelerazione e Ventola (Fan) sono state rese cliccabili. Premendole, si aprono specifiche pagine/modali separate che permettono (o permetteranno) di variare i relativi valori.

### 3. Allineamenti e Design Pixel-Perfect
- **Allineamento Divisori:** È stato corretto un disallineamento visivo di 1 pixel tra la linea verticale superiore (che divide Velocità/Accel da Fan/Host) e la linea verticale inferiore (che divide i bottoni Nozzle e Bed). Lo spostamento di -1px della linea superiore ha compensato il bordo interno del contenitore.
- **Sperimentazione Design:** È stato fatto un tentativo di rendere le celle dati (Speed, Fan, ecc.) visivamente simili a "tasti smussati" (sfondo scuro, bordi arrotondati), ma la modifica è stata annullata su richiesta per mantenere un look più pulito e trasparente.

## 🐛 Bug Risolti

### Blocco alla Schermata di Boot (Boot Logo Freeze)
- **Sintomo:** Il dispositivo si bloccava costantemente all'accensione mostrando solo il logo iniziale.
- **Causa:** Esaurimento della memoria heap di LVGL. L'uso combinato di `clip_corner = true` e `LV_RADIUS_CIRCLE` su elementi molto grandi (come il TileView) causava calcoli infiniti/estremamente pesanti per le maschere di ritaglio in LVGL v8, consumando tutti i 48KB di memoria assegnata e mandando la scheda in crash (assert).
- **Soluzione:**
  1. Aumentata la memoria dedicata a LVGL (`LV_MEM_SIZE`) da 48KB a 80KB in `lv_conf.h`.
  2. Rimossi tutti i comandi `lv_obj_set_style_clip_corner()` non necessari.
  3. Sostituito `LV_RADIUS_CIRCLE` con valori di raggio espliciti in pixel (es. 108 per la card centrale, 120 per le modali) per evitare bug di calcolo maschera.
  4. Ottimizzate le funzioni di callback dello scroll e rimossi log di debug pesanti.

## 🔌 Gestione Flashing e COM Port
Durante le sessioni, la porta seriale della scheda è cambiata (da COM6 a COM4 e viceversa) ed è stato riscontrato un errore di "Write timeout" durante l'upload. Questo è stato risolto forzando la porta corretta o riavviando fisicamente la scheda (tasti BOOT/RESET) per farla entrare in modalità bootloader.
