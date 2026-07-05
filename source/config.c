/* config.c -- runtime screen size
 *
 * Burger Shop (com.gobit.burgershop) Switch port.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// The game requires the touchscreen, which only exists in handheld mode, so the
// port is locked to the handheld panel (1280x720). There are no user-facing
// resolution options -- docked would be unplayable (no touch).
int screen_width  = 1280;
int screen_height = 720;
