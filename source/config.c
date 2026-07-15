/* config.c -- runtime screen size
 *
 * Burger Shop (com.gobit.burgershop) Switch port.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

// Render at 1080p in both handheld and docked (set_screen_size in main.c). The
// touchscreen (handheld only) reports in 1280x720 panel space; nx_pointer scales
// touches into this render space, and its stick/gyro/mouse cursor makes docked
// playable too.
int screen_width  = 1920;
int screen_height = 1080;
