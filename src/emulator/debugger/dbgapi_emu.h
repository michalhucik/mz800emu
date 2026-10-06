/*
 * dbgapi_emu.h — API pro stranu Emulátoru (server)
 *
 * Funkce v tomto headeru volá emulátorové vlákno:
 * - Kontrola a zpracování CMDRQ fronty (příkazy z UI)
 * - Odesílání MSG notifikací do UI
 *
 * Includovat pouze v emulátorové části kódu (src/emulator/).
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */
#ifndef DBGAPI_EMU_H
#define DBGAPI_EMU_H

#include "main.h"
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include "app/app_thread.h"
#include "dbgapi_cmdrq.h"
#include "dbgapi_msg.h"

/* Globální fronta CMDRQ — definice v dbgapi.c */
extern st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;


#ifdef __cplusplus
extern "C"
{
#endif

/* ============================================================================
 * INICIALIZACE A UKONČENÍ
 *
 * Volat před spuštěním emulačního vlákna (init) a po jeho ukončení (destroy).
 * Init alokuje mutexy a condition variables pro všechny sloty fronty.
 * Destroy je uvolní.
 * ============================================================================ */

/*
 * Inicializace CMDRQ fronty.
 * Alokuje mutexy a condition variables pro všechny sloty i frontu.
 * Nastaví head = tail = 0, reply_state = NONE.
 */
void dbgapi_init(st_DBGAPI_CMDRQ_QUEUE *queue);

/*
 * Uvolnění zdrojů CMDRQ fronty.
 * Dealokuje všechny mutexy a condition variables.
 */
void dbgapi_destroy(st_DBGAPI_CMDRQ_QUEUE *queue);


/* ============================================================================
 * KONTROLA A ZPRACOVÁNÍ FRONTY (EMU strana)
 *
 * Emulátor kontroluje frontu v pravidelných intervalech:
 * - Za běhu: jednou za snímek (≈20ms)
 * - V pause: ~1ms cyklus s APP_COND_WAIT_TIMEOUT_MS na queue_cond
 *
 * Typický vzor:
 *   while (dbgapi_emu_has_pending(queue))
 *   {
 *       st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue(queue);
 *       dbgapi_emu_dispatch(rq);
 *       bool blocking = (rq->cmd & DBGAPI_CMDFLAG_BLOCKING) != 0;
 *       dbgapi_emu_complete(rq);
 *       if (!blocking) break;
 *   }
 * ============================================================================ */

/*
 * Zjistí, zda jsou ve frontě čekající příkazy.
 * Thread-safe (zamyká queue_mutex).
 * Vrací true pokud fronta není prázdná.
 */
bool dbgapi_emu_has_pending(st_DBGAPI_CMDRQ_QUEUE *queue);

/*
 * Vyjme příkaz z fronty (posune head).
 * Thread-safe (zamyká queue_mutex).
 * Vrací ukazatel na slot, nebo NULL pokud je fronta prázdná.
 *
 * Sloty zrušené odesílatelem po timeoutu (DBGAPI_CMDSTATE_CANCELLED)
 * přeskočí a uvolní - takový příkaz se neprovede. Vrácený slot označí
 * jako vyzvednutý (dequeued = true); od té chvíle ho odesílatel nezruší
 * a čeká na dbgapi_emu_complete().
 *
 * POZOR: Vrácený slot je platný do volání dbgapi_emu_complete().
 * Emulátor musí zpracovat příkaz a zavolat complete() před dalším dequeue().
 */
st_DBGAPI_CMDRQ *dbgapi_emu_dequeue(st_DBGAPI_CMDRQ_QUEUE *queue);

/*
 * Zpracování příkazu — dispatch na příslušný handler.
 * Volat po dequeue() s platným slotem.
 *
 * Interně provede switch přes (rq->cmd & DBGAPI_CMD_MASK):
 * - Nastaví rq->success na true/false
 * - Zapíše odpověď do rq->result_ptr (pokud je relevantní)
 */
void dbgapi_emu_dispatch(st_DBGAPI_CMDRQ *rq);

/*
 * Signalizace dokončení zpracování příkazu.
 * Nastaví cmd_state = PROCESSED a signalizuje slot->cond.
 * UI vlákno, které čeká na odpověď, se probudí.
 *
 * Volat po dbgapi_emu_dispatch() (i pokud dispatch selhal).
 */
void dbgapi_emu_complete(st_DBGAPI_CMDRQ *rq);

/*
 * Čekání na nový příkaz ve frontě (pro pause režim).
 * Blokuje vlákno do příchodu nového příkazu nebo do timeoutu.
 * timeout_ms: maximální čekání v milisekundách.
 * Vrací true pokud ve frontě je příkaz, false pokud vypršel timeout.
 *
 * Typické použití v pause loop:
 *   while (paused)
 *   {
 *       if (dbgapi_emu_wait_for_cmd(queue, 1))
 *       {
 *           st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue(queue);
 *           dbgapi_emu_dispatch(rq);
 *           dbgapi_emu_complete(rq);
 *       }
 *   }
 */
bool dbgapi_emu_wait_for_cmd(st_DBGAPI_CMDRQ_QUEUE *queue, int timeout_ms);

/*
 * Nastavení ochranného příznaku — emulátor se ukončuje.
 * Po zavolání UI vlákno ví, že nemá smysl posílat další příkazy.
 */
void dbgapi_emu_set_ending(st_DBGAPI_CMDRQ_QUEUE *queue);


/* ============================================================================
 * ODESÍLÁNÍ MSG NOTIFIKACÍ (EMU → UI) PŘES DISPATCHER
 *
 * dbgapi nezná SDL ani sdlapp - thread switch z EMU vlákna do UI vlákna
 * řeší dispatcher zaregistrovaný UI vrstvou přes
 * dbgapi_emu_register_msg_dispatcher(). Implementace dispatcheru
 * (= SDL custom event nebo jiná inter-thread fronta) je v UI vrstvě
 * (src/ui-imgui/debugger/dbgapi_dispatcher.cpp).
 *
 * Pro jednoduché MSG (bez dat): data = NULL.
 * Pro MSG s daty: data se alokují na heapu (g_new0), vlastnictví předáno
 * dispatcheru/listeneru, který je uvolní přes g_free().
 * ============================================================================ */

/**
 * @brief Typ dispatcheru pro doručení MSG z EMU vlákna do UI.
 *
 * Dispatcher je registrován UI vrstvou přes
 * dbgapi_emu_register_msg_dispatcher() při startup. Volán z EMU vlákna
 * při dbgapi_emu_send_msg(). Implementace musí zajistit thread switch
 * (typicky SDL custom event nebo jiná inter-thread fronta) a doručit
 * MSG do UI vlákna, kde se zpracuje.
 *
 * Vlastnictví parametru `data` přebírá dispatcher (= zodpovědný za
 * uvolnění přes g_free() po doručení nebo zahození).
 *
 * @param msg        Typ MSG (en_DBGAPI_MSG).
 * @param data       Volitelná heap-alokovaná data; vlastnictví předáno.
 * @param user_data  Pointer předaný při registraci.
 */
typedef void (*dbgapi_msg_dispatcher_t)(en_DBGAPI_MSG msg,
                                         st_DBGAPI_MSG_DATA *data,
                                         void *user_data);

/**
 * @brief Registrovat dispatcher pro doručení MSG do UI.
 *
 * Volat z UI vlákna při startup. Pokud žádný dispatcher zaregistrován,
 * dbgapi_emu_send_msg() zahodí MSG včetně dat. Lze přepsat - poslední
 * registrace vyhrává; pro odregistraci předat NULL.
 *
 * @param dispatcher  Callback funkce, nebo NULL pro odregistraci.
 * @param user_data   Pointer předaný do dispatcher při každém volání.
 *
 * @note dbgapi.c (EMU strana) nesmí znát SDL/sdlapp - thread switch
 *       řeší výhradně dispatcher v UI vrstvě.
 */
void dbgapi_emu_register_msg_dispatcher(dbgapi_msg_dispatcher_t dispatcher,
                                          void *user_data);

/**
 * @brief Odeslání MSG notifikace z EMU do UI přes registrovaný dispatcher.
 *
 * Volá registrovaný dispatcher (přes dbgapi_emu_register_msg_dispatcher)
 * a předává mu vlastnictví dat. Pokud dispatcher není zaregistrovaný,
 * MSG se tiše zahodí a data uvolní přes g_free().
 *
 * @param msg   Typ zprávy (en_DBGAPI_MSG).
 * @param data  Volitelná data - NULL pro jednoduché zprávy, nebo
 *              heap-alokovaná st_DBGAPI_MSG_DATA. Vlastnictví předáno
 *              dispatcheru (případně g_free() pokud žádný dispatcher).
 *
 * @note Volat výhradně z EMU vlákna.
 */
void dbgapi_emu_send_msg(en_DBGAPI_MSG msg, st_DBGAPI_MSG_DATA *data);


#ifdef __cplusplus
}
#endif

#endif /* DBGAPI_EMU_H */
