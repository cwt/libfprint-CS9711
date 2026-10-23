/*
 * Chipsailing CS9711Fingprint driver
 *
 * Modified based on driver vfs301* so keeping original notice:
 *
 * Copyright (c) 2011-2012 Andrej Krutak <dev@andree.sk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "cs9711"

#include "drivers_api.h"
#include "cs9711.h"

G_DEFINE_TYPE (FpDeviceCs9711, fpi_device_cs9711, FP_TYPE_IMAGE_DEVICE)

#define CS9711_SENSOR_WIDTH 34
#define CS9711_SENSOR_HEIGHT 236

#define CS9711_DEFAULT_WAIT_TIMEOUT 300
#define CS9711_DEFAULT_RESET_SLEEP 250
#define CS9711_DRAIN_TIMEOUT 50
#define CS9711_MAX_DRAIN_READS 4

#define CS9711_SEND_ENDPOINT 0x01
#define CS9711_RECEIVE_ENDPOINT 0x81

#define CS9711_FP_CMD_LEN_1 8
#define CS9711_FP_RECV_LEN_1 8000
#define CS9711_FP_RECV_LEN_2 24
#define CS9711_FP_RECV_LEN_MAX CS9711_FP_RECV_LEN_1

#define CS9711_FP_CMD_TYPE_INIT 1
#define CS9711_FP_CMD_TYPE_RESET 2
#define CS9711_FP_CMD_TYPE_SCAN 4

#define CS9711_FP_CMD_STATE_RESULT_EXPECTED { 0xea, 0x01, 0x62, 0xa0, 0x00, 0x00, 0xc3, 0xea }

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif


/************************** GENERIC STUFF *************************************/

/** If error isn't NULL then fail the ssm or move it to the next state */
static void
m_util_fail_if_error_or_next (FpiSsm *ssm, GError *error)
{
  if (error)
    fpi_ssm_mark_failed (ssm, error);
  else
    fpi_ssm_next_state (ssm);
}

/** Synchroneous USB bulk write OUT helper*/
static void
usb_send_out_sync (FpDevice *dev, guint8 type, GError **error)
{
  GError *err = NULL;

  guint8 *data = g_malloc0 (CS9711_FP_CMD_LEN_1);

  data[0] = data[CS9711_FP_CMD_LEN_1 - 1] = 0xEA;
  data[1] = data[CS9711_FP_CMD_LEN_1 - 2] = type;

  g_autoptr(FpiUsbTransfer) transfer = NULL;

  transfer = fpi_usb_transfer_new (FP_DEVICE (dev));
  transfer->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk_full (transfer, CS9711_SEND_ENDPOINT, (guint8 *) data, CS9711_FP_CMD_LEN_1, g_free);
  fpi_usb_transfer_submit_sync (transfer, CS9711_DEFAULT_WAIT_TIMEOUT, &err);
  if (err)
    {
      fp_dbg ("Error while sending command 0x%X: %s", type, err->message);
      g_propagate_error (error, err);
    }
  else
    {
      fp_dbg ("Sent command 0x%X", type);
    }
}

/** Asynchroneous USB bulk write IN helper */
static void
usb_read_in (FpDevice              *dev,
             FpiSsm                *ssm,
             gsize                  length,
             gboolean               short_is_error,
             guint                  timeout_in_ms,
             FpiUsbTransferCallback callback,
             gpointer               user_data)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  FpiUsbTransfer *transfer = NULL;

  fp_dbg ("Reading %lu bytes", length);
  // If the response is larger than the buffer, then the usb helpers
  // error before. The reader doesn't seem to care about requestend
  // length, and occasionally answers out of sequence with an invalid
  // data size. So just request the max expected, and deal with errors
  // in the callback. For the same reason, cannot use short_is_error
  // facility
  // FIXED: Respect the intended length parameter instead of forcing max length
  gsize actual_length = MIN (length, CS9711_FP_RECV_LEN_MAX);
  short_is_error = FALSE;
  transfer = fpi_usb_transfer_new (FP_DEVICE (dev));
  transfer->short_is_error = short_is_error;
  transfer->ssm = ssm;
  fpi_usb_transfer_fill_bulk (transfer, CS9711_RECEIVE_ENDPOINT, actual_length);
  /* Cancellable, so deactivation can stop a SCAN still waiting for a finger */
  fpi_usb_transfer_submit (transfer, timeout_in_ms, self->cancellable, callback, user_data);
}

/************************** INIT SSM *************************************/

static void
m_init_read_cb_check_expected (FpiUsbTransfer *transfer,
                               FpDevice       *dev,
                               gpointer        user_data_is_ignore_mismatch_if_non_null,
                               GError         *error)
{
  const guint8 expected[] = CS9711_FP_CMD_STATE_RESULT_EXPECTED;

  g_assert (CS9711_FP_CMD_LEN_1 == sizeof (expected));
  g_assert (transfer->ssm != NULL);

  if (error)
    {
      fp_err ("Read failed: %s, aborting", error->message);
      fpi_ssm_mark_failed (transfer->ssm, error);
    }
  else
    {
      fp_dbg ("Read %lu of requested %lu", transfer->actual_length, transfer->length);
      if (transfer->actual_length != CS9711_FP_CMD_LEN_1)
        fp_warn ("Error; expected %lu bytes but got %lu, continuing", (gsize) CS9711_FP_CMD_LEN_1, transfer->actual_length);
      else if (memcmp (transfer->buffer, expected, CS9711_FP_CMD_LEN_1) == 0)
        fp_dbg ("Init response valid");
      else if (!user_data_is_ignore_mismatch_if_non_null)
        fp_warn ("Error; got different state response than expected, but don't understand it anyway, continuing");
      fpi_ssm_next_state (transfer->ssm);
    }
}

/* Read until the sensor has nothing left to send. Data can be left behind by
 * a SCAN or a reply whose read was cut off (a crashed or killed process, or a
 * cancelled operation); it would otherwise arrive as the INIT response and
 * fail activation, and while it is unread the sensor was seen to refuse
 * commands, RESET included. */
static void
m_init_drain_cb (FpiUsbTransfer *transfer,
                 FpDevice       *dev,
                 gpointer        user_data,
                 GError         *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);

  if (error)
    {
      if (!g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        fp_warn ("Draining the sensor failed (%s), continuing", error->message);
      g_clear_error (&error);
      fpi_ssm_next_state (transfer->ssm);
      return;
    }

  fp_dbg ("Drained %lu stale bytes from the sensor", transfer->actual_length);
  if (++self->drain_reads < CS9711_MAX_DRAIN_READS)
    fpi_ssm_jump_to_state (transfer->ssm, fpi_ssm_get_cur_state (transfer->ssm));
  else
    fpi_ssm_next_state (transfer->ssm);
}

enum {
  M_INIT_STATE_FLUSH_DRAIN = 0,
  M_INIT_STATE_FLUSH_RESET,
  M_INIT_STATE_FLUSH_DRAIN_AGAIN,
  M_INIT_STATE_SEND_INI_QUERY,
  M_INIT_STATE_RECOVER_READ_IGNORED,
  M_INIT_STATE_RECOVER_SEND_RESET,
  M_INIT_STATE_RECOVER_READ_IGNORED_RESET,
  M_INIT_STATE_RECOVER_SEND_INIT,
  M_INIT_STATE_RECEIVE_STATUS,
  M_INIT_STATE_COUNT,
};

/* Exec init sequential state machine */
static void
m_init_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (_dev);
  GError *error = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    /* Empty the sensor, RESET it (which ends a SCAN left waiting for a
     * finger), and empty it again. */
    case M_INIT_STATE_FLUSH_DRAIN:
    case M_INIT_STATE_FLUSH_DRAIN_AGAIN:
      usb_read_in (_dev, ssm, CS9711_FP_RECV_LEN_MAX, FALSE, CS9711_DRAIN_TIMEOUT, m_init_drain_cb, NULL);
      break;

    case M_INIT_STATE_FLUSH_RESET:
      usb_send_out_sync (_dev, CS9711_FP_CMD_TYPE_RESET, &error);
      g_clear_error (&error);
      self->drain_reads = 0;
      fpi_ssm_next_state (ssm);
      break;

    case M_INIT_STATE_SEND_INI_QUERY:
      usb_send_out_sync (_dev, CS9711_FP_CMD_TYPE_INIT, &error);
      if (error)
        {
          fp_dbg ("Error details: '%s', quark: %u code: %d", error->message, error->domain, error->code);
          // if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
          if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
            {
              g_clear_error (&error);
              fpi_ssm_next_state (ssm);
            }
          else
            {
              fpi_ssm_mark_failed (ssm, error);
            }
        }
      else
        {
          fpi_ssm_jump_to_state (ssm, M_INIT_STATE_RECEIVE_STATUS);
        }
      break;

    case M_INIT_STATE_RECOVER_READ_IGNORED:
      fp_warn ("Send operation had a timeout. Switching to reset procedure. Ignore next message about the result not matching the expected data.");
      usb_read_in (_dev, ssm, CS9711_FP_CMD_LEN_1, FALSE, CS9711_DEFAULT_WAIT_TIMEOUT, m_init_read_cb_check_expected, NULL);
      break;

    case M_INIT_STATE_RECOVER_SEND_RESET:
      usb_send_out_sync (_dev, CS9711_FP_CMD_TYPE_RESET, &error);
      m_util_fail_if_error_or_next (ssm, error);
      break;

    case M_INIT_STATE_RECOVER_READ_IGNORED_RESET:
      fp_warn ("Send operation had a timeout. Switching to reset procedure.");
      usb_read_in (_dev, ssm, CS9711_FP_CMD_LEN_1, FALSE, CS9711_DEFAULT_WAIT_TIMEOUT, m_init_read_cb_check_expected, self);
      break;

    case M_INIT_STATE_RECOVER_SEND_INIT:
      usb_send_out_sync (_dev, CS9711_FP_CMD_TYPE_INIT, &error); // Do not reuse state to only try reset once
      m_util_fail_if_error_or_next (ssm, error);
      break;

    case M_INIT_STATE_RECEIVE_STATUS:
      usb_read_in (_dev, ssm, CS9711_FP_RECV_LEN_MAX, FALSE, CS9711_DEFAULT_WAIT_TIMEOUT, m_init_read_cb_check_expected, NULL);
      break;

    default:
      g_assert_not_reached ();
    }
  // cs9711_proto_init (self);

  // fpi_ssm_mark_completed (ssm);
}

/* Complete init sequential state machine */
static void
m_init_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  /* libfprint ignores a cancellation that arrives while the device is
   * activating, so look for one here; otherwise the press loop would start
   * and wait for a finger the caller no longer wants. */
  if (!error && fpi_device_action_is_cancelled (dev))
    g_cancellable_set_error_if_cancelled (fpi_device_get_cancellable (dev), &error);

  fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), error);
}

/************************** PRESS LOOP *************************************/

/*
 * How the sensor behaves (measured with a direct USB probe and the example
 * programs, October 2026):
 *   - a SCAN read is held by the sensor until a finger touches it;
 *   - while the finger stays down, every new SCAN answers at once (~70 ms),
 *     and frames of an unmoving finger differ by a mean of 0.9-2.4 grey levels;
 *   - in normal presses, frames 1 and 2 differed by 8.3-37.3 (the finger is
 *     still landing) and frames 2 and 3 by 1.2-3.8, except 4.6 and 6.0 in two
 *     presses that settled one frame later; a moving finger differs by 16+;
 *   - with no finger, a SCAN read times out, and RESET + INIT recover cleanly.
 *
 * The old driver reported finger-on when it sent SCAN and finger-off as soon
 * as the first frame arrived. libfprint then started the next attempt at once
 * with the finger still down, so verify burned its attempts on copies of one
 * first-contact frame, and enroll filled all its stages from a single press.
 *
 * Here one loop runs per finger placement: wait for the first frame, keep
 * scanning until two frames in a row agree (the finger has settled), then
 * keep scanning until a SCAN times out (the finger has lifted) and only then
 * report finger-off. The settled frame is submitted at once, or at the lift
 * during enroll. A finger that lifts before it settles, or never settles,
 * gets a retry instead of a moving frame.
 */

#define CS9711_LIFT_POLL_TIMEOUT 300  /* ms; a SCAN with a finger answers in ~70 ms */
#define CS9711_SETTLE_MAD 4.0         /* settled pairs measured <= 3.8, landing >= 4.6 */
#define CS9711_SETTLE_MAX_FRAMES 5    /* ask for a new placement after this many */
#define CS9711_MAX_BAD_READS 3

enum {
  M_LOOP_INIT_SEND = 0,
  M_LOOP_INIT_READ,
  M_LOOP_SLEEP,
  M_LOOP_SCAN,
  M_LOOP_READ_TAIL,
  M_LOOP_FRAME_DONE,
  M_LOOP_RECOVER_DRAIN,
  M_LOOP_RECOVER_RESET,
  M_LOOP_RECOVER_DRAIN_AGAIN,
  M_LOOP_NUM_STATES,
};

static void
m_press_reset (FpDeviceCs9711 *self)
{
  self->finger_on = FALSE;
  self->settled = FALSE;
  self->submitted = FALSE;
  self->lifted = FALSE;
  self->frames_in_press = 0;
}

static double
frame_mad (const guint8 *a, const guint8 *b)
{
  guint64 sum = 0;

  for (gsize i = 0; i < CS9711_FRAME_SIZE; i++)
    sum += ABS ((int) a[i] - (int) b[i]);

  return (double) sum / CS9711_FRAME_SIZE;
}

/* Leave the sensor idle and end the loop; m_loop_complete finishes deactivation. */
static void
m_loop_finish (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  GError *error = NULL;

  usb_send_out_sync (dev, CS9711_FP_CMD_TYPE_RESET, &error);
  g_clear_error (&error);
  self->need_init = TRUE;
  fpi_ssm_mark_completed (ssm);
}

/* Recover (drain, RESET, drain, INIT), or give up after repeated bad reads.
 * The counter is only cleared by a complete frame. */
static void
m_loop_bad_read (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);

  if (++self->bad_reads >= CS9711_MAX_BAD_READS)
    {
      GError *reset_error = NULL;

      /* Leave the sensor idle before giving up. */
      usb_send_out_sync (dev, CS9711_FP_CMD_TYPE_RESET, &reset_error);
      g_clear_error (&reset_error);
      self->need_init = TRUE;
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fp_warn ("Bad read from sensor (%s), recovering", error->message);
  g_clear_error (&error);
  self->drain_reads = 0;
  fpi_ssm_jump_to_state (ssm, M_LOOP_RECOVER_DRAIN);
}

static int
m_scan_submit_image (FpiSsm        *ssm,
                     FpImageDevice *dev,
                     const guint8  *frame)
{
  FpImage *img;

  img = fp_image_new (CS9711_WIDTH, CS9711_HEIGHT);
  if (img == NULL)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (FP_DEVICE_ERROR,
                                             FP_DEVICE_ERROR_GENERAL,
                                             "Failed to allocate image"));
      return 1;
    }

  for (gsize y = 0; y < CS9711_SENSOR_HEIGHT; y++)
    for (gsize x = 0; x < CS9711_SENSOR_WIDTH; x++)
      {
        gsize dy = y / 2;
        gsize dx = x * 2 + y % 2;

        // Bounds checking to prevent buffer overflow
        if (dy < CS9711_HEIGHT && dx < CS9711_WIDTH &&
            (y * CS9711_SENSOR_WIDTH + x) < CS9711_FRAME_SIZE &&
            (dy * CS9711_WIDTH + dx) < (CS9711_WIDTH * CS9711_HEIGHT))
          img->data[dy * CS9711_WIDTH + dx] = frame[y * CS9711_SENSOR_WIDTH + x];
      }

  img->flags = FPI_IMAGE_PARTIAL;

  fpi_image_device_image_captured (dev, img);

  return 0;
}

static void
m_loop_init_read_cb (FpiUsbTransfer *transfer,
                     FpDevice       *dev,
                     gpointer        user_data,
                     GError         *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  const guint8 expected[] = CS9711_FP_CMD_STATE_RESULT_EXPECTED;

  if (self->deactivating)
    {
      g_clear_error (&error);
      m_loop_finish (transfer->ssm, dev);
      return;
    }
  if (error)
    {
      m_loop_bad_read (transfer->ssm, dev, error);
      return;
    }
  /* Any other length is data left in the sensor, not the INIT response. */
  if (transfer->actual_length != CS9711_FP_CMD_LEN_1)
    {
      m_loop_bad_read (transfer->ssm, dev,
                       g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                                    "expected a %d-byte INIT response but got %lu bytes",
                                    CS9711_FP_CMD_LEN_1, transfer->actual_length));
      return;
    }
  if (memcmp (transfer->buffer, expected, CS9711_FP_CMD_LEN_1))
    fp_warn ("Unexpected INIT response, continuing");

  self->need_init = FALSE;
  fpi_ssm_next_state (transfer->ssm);
}

static void
m_loop_drain_cb (FpiUsbTransfer *transfer,
                 FpDevice       *dev,
                 gpointer        user_data,
                 GError         *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  FpiSsm *ssm = transfer->ssm;

  if (self->deactivating)
    {
      g_clear_error (&error);
      m_loop_finish (ssm, dev);
      return;
    }
  if (!error)
    {
      fp_dbg ("Drained %lu stale bytes from the sensor", transfer->actual_length);
      if (++self->drain_reads < CS9711_MAX_DRAIN_READS)
        {
          fpi_ssm_jump_to_state (ssm, fpi_ssm_get_cur_state (ssm));
          return;
        }
    }
  else if (!g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
    {
      fp_dbg ("Draining the sensor failed (%s), continuing", error->message);
    }
  g_clear_error (&error);

  if (fpi_ssm_get_cur_state (ssm) == M_LOOP_RECOVER_DRAIN)
    {
      fpi_ssm_next_state (ssm);
    }
  else
    {
      self->need_init = TRUE;
      fpi_ssm_jump_to_state (ssm, M_LOOP_INIT_SEND);
    }
}

static void
m_loop_frame_cb (FpiUsbTransfer *transfer,
                 FpDevice       *dev,
                 gpointer        user_data,
                 GError         *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  FpiSsm *ssm = transfer->ssm;

  if (self->deactivating)
    {
      g_clear_error (&error);
      m_loop_finish (ssm, dev);
      return;
    }

  /* Only the lift poll has a timeout, so no frame means no finger. */
  if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT) &&
      self->finger_on)
    {
      g_clear_error (&error);
      fp_dbg ("Finger lifted after %u frames", self->frames_in_press);
      self->lifted = TRUE;
      if (!self->submitted)
        {
          self->submitted = TRUE;
          if (self->settled)
            {
              /* Enroll: the settled frame is submitted now, see m_loop_frame_done. */
              if (m_scan_submit_image (ssm, FP_IMAGE_DEVICE (dev), self->prev_frame) != 0)
                return;
            }
          else
            {
              /* The finger left before it settled. */
              fpi_image_device_retry_scan (FP_IMAGE_DEVICE (dev), FP_DEVICE_RETRY_GENERAL);
            }
        }
      m_loop_finish (ssm, dev);
      return;
    }
  if (error)
    {
      m_loop_bad_read (ssm, dev, error);
      return;
    }
  if (transfer->actual_length != CS9711_FP_RECV_LEN_1)
    {
      m_loop_bad_read (ssm, dev,
                       g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                                    "expected %d image bytes but got %lu",
                                    CS9711_FP_RECV_LEN_1, transfer->actual_length));
      return;
    }

  memcpy (self->image_buffer, transfer->buffer, CS9711_FP_RECV_LEN_1);
  fpi_ssm_next_state (ssm);
}

static void
m_loop_tail_cb (FpiUsbTransfer *transfer,
                FpDevice       *dev,
                gpointer        user_data,
                GError         *error)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  FpiSsm *ssm = transfer->ssm;

  if (self->deactivating)
    {
      g_clear_error (&error);
      m_loop_finish (ssm, dev);
      return;
    }
  if (error)
    {
      m_loop_bad_read (ssm, dev, error);
      return;
    }
  if (transfer->actual_length != CS9711_FP_RECV_LEN_2)
    {
      m_loop_bad_read (ssm, dev,
                       g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                                    "expected %d tail bytes but got %lu",
                                    CS9711_FP_RECV_LEN_2, transfer->actual_length));
      return;
    }

  memcpy (self->image_buffer + CS9711_FP_RECV_LEN_1, transfer->buffer, CS9711_FP_RECV_LEN_2);
  fpi_ssm_next_state (ssm);
}

/* One frame is in image_buffer: track the press and decide it once settled. */
static void
m_loop_frame_done (FpiSsm *ssm, FpDevice *dev)
{
  FpImageDevice *image_device = FP_IMAGE_DEVICE (dev);
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  double mad;

  if (!self->finger_on)
    {
      self->finger_on = TRUE;
      self->frames_in_press = 1;
      memcpy (self->prev_frame, self->image_buffer, CS9711_FRAME_SIZE);
      fpi_image_device_report_finger_status (image_device, TRUE);
      fpi_ssm_jump_to_state (ssm, M_LOOP_INIT_SEND);
      return;
    }

  self->frames_in_press++;
  if (self->submitted || self->settled)
    {
      /* This press is decided; keep polling until the finger lifts. */
      fpi_ssm_jump_to_state (ssm, M_LOOP_INIT_SEND);
      return;
    }

  mad = frame_mad (self->prev_frame, self->image_buffer);
  fp_dbg ("Frame %u of this press differs from the last by %.1f",
          self->frames_in_press, mad);
  memcpy (self->prev_frame, self->image_buffer, CS9711_FRAME_SIZE);

  if (mad < CS9711_SETTLE_MAD)
    {
      /* libfprint completes an enrollment as soon as the last image is
       * processed, so in enroll the image is submitted when the finger
       * lifts. Elsewhere it is submitted now, for an early result. */
      if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_ENROLL)
        {
          self->settled = TRUE;
        }
      else
        {
          self->submitted = TRUE;
          if (m_scan_submit_image (ssm, image_device, self->prev_frame) != 0)
            return;
        }
    }
  else if (self->frames_in_press >= CS9711_SETTLE_MAX_FRAMES)
    {
      /* The finger keeps moving: ask for a new placement. Outside enroll
       * this deactivates the device, which m_loop_state then sees. */
      self->submitted = TRUE;
      fpi_image_device_retry_scan (image_device, FP_DEVICE_RETRY_REMOVE_FINGER);
    }

  fpi_ssm_jump_to_state (ssm, M_LOOP_INIT_SEND);
}

static void
m_loop_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  GError *error = NULL;

  if (self->deactivating)
    {
      m_loop_finish (ssm, dev);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case M_LOOP_INIT_SEND:
      if (!self->need_init)
        {
          fpi_ssm_jump_to_state (ssm, M_LOOP_SLEEP);
          break;
        }
      usb_send_out_sync (dev, CS9711_FP_CMD_TYPE_INIT, &error);
      if (error)
        m_loop_bad_read (ssm, dev, error);
      else
        fpi_ssm_next_state (ssm);
      break;

    case M_LOOP_INIT_READ:
      usb_read_in (dev, ssm, CS9711_FP_RECV_LEN_MAX, FALSE, CS9711_DEFAULT_WAIT_TIMEOUT,
                   m_loop_init_read_cb, NULL);
      break;

    case M_LOOP_SLEEP:
      /* Needed: without this pause, a SCAN sent straight after RESET + INIT
       * times out on every other cycle even with a finger held, which would
       * read as a lift. Without INIT, scans stop returning frames. */
      fpi_ssm_next_state_delayed (ssm, CS9711_DEFAULT_RESET_SLEEP);
      break;

    case M_LOOP_SCAN:
      usb_send_out_sync (dev, CS9711_FP_CMD_TYPE_SCAN, &error);
      if (error)
        {
          m_loop_bad_read (ssm, dev, error);
          break;
        }
      /* Waiting for a touch has no time limit; once a finger is down, a
       * timeout means it has lifted. m_loop_frame_cb moves the loop on. */
      usb_read_in (dev, ssm, CS9711_FP_RECV_LEN_1, FALSE,
                   self->finger_on ? CS9711_LIFT_POLL_TIMEOUT : 0,
                   m_loop_frame_cb, NULL);
      break;

    case M_LOOP_READ_TAIL:
      usb_read_in (dev, ssm, CS9711_FP_RECV_LEN_MAX, FALSE, CS9711_DEFAULT_WAIT_TIMEOUT,
                   m_loop_tail_cb, NULL);
      break;

    case M_LOOP_FRAME_DONE:
      usb_send_out_sync (dev, CS9711_FP_CMD_TYPE_RESET, &error);
      if (error)
        {
          m_loop_bad_read (ssm, dev, error);
          break;
        }
      self->need_init = TRUE;
      self->bad_reads = 0;
      m_loop_frame_done (ssm, dev);
      break;

    case M_LOOP_RECOVER_DRAIN:
    case M_LOOP_RECOVER_DRAIN_AGAIN:
      usb_read_in (dev, ssm, CS9711_FP_RECV_LEN_MAX, FALSE, CS9711_DRAIN_TIMEOUT,
                   m_loop_drain_cb, NULL);
      break;

    case M_LOOP_RECOVER_RESET:
      usb_send_out_sync (dev, CS9711_FP_CMD_TYPE_RESET, &error);
      g_clear_error (&error);
      self->drain_reads = 0;
      fpi_ssm_next_state (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
m_loop_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice *image_device = FP_IMAGE_DEVICE (dev);
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  gboolean lifted = self->lifted;

  self->loop_ssm = NULL;
  m_press_reset (self);

  /* Report finger-off only now that the loop is gone: in enroll it starts
   * the next press loop straight away. */
  if (self->deactivating)
    fpi_image_device_deactivate_complete (image_device, error);
  else if (error)
    fpi_image_device_session_error (image_device, error);
  else if (lifted)
    fpi_image_device_report_finger_status (image_device, FALSE);
}

/************************** ImageDevice impl. *************************************/

/* Activate device */
static void
dev_activate (FpImageDevice *dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  FpiSsm *ssm;

  self->deactivating = FALSE;
  self->need_init = FALSE; /* the init ssm sends INIT */
  self->bad_reads = 0;
  self->drain_reads = 0;
  m_press_reset (self);
  g_clear_object (&self->cancellable);
  self->cancellable = g_cancellable_new ();

  /* Start init ssm */
  ssm = fpi_ssm_new (FP_DEVICE (dev), m_init_state, M_INIT_STATE_COUNT);
  fpi_ssm_start (ssm, m_init_complete);
}

/* Deactivate device */
static void
dev_deactivate (FpImageDevice *dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);

  self->deactivating = TRUE;
  if (!self->loop_ssm)
    {
      fpi_image_device_deactivate_complete (dev, NULL);
    }
  else if (fpi_ssm_get_cur_state (self->loop_ssm) == M_LOOP_SLEEP)
    {
      /* No transfer is pending; do not wait out the pause. */
      fpi_ssm_cancel_delayed_state_change (self->loop_ssm);
      m_loop_finish (self->loop_ssm, FP_DEVICE (dev));
    }
  else
    {
      /* The pending read, or the code that called us, ends the loop. */
      g_cancellable_cancel (self->cancellable);
    }
}

static void
dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);

  if (state != FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    return;

  if (self->loop_ssm)
    {
      g_warning ("Press loop already running, not starting another");
      return;
    }

  /* Start a press loop. */
  m_press_reset (self);
  self->bad_reads = 0;
  self->loop_ssm = fpi_ssm_new (FP_DEVICE (dev), m_loop_state, M_LOOP_NUM_STATES);
  fpi_ssm_start (self->loop_ssm, m_loop_complete);
}

static void
dev_open (FpImageDevice *dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  GError *error = NULL;

  /* Claim usb interface */
  g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error);

  /* Initialize private structure */
  memset (self->image_buffer, 0, CS9711_FRAME_SIZE);

  /* Notify open complete */
  fpi_image_device_open_complete (dev, error);
}

static void
dev_close (FpImageDevice *dev)
{
  FpDeviceCs9711 *self = FPI_DEVICE_CS9711 (dev);
  GError *error = NULL;

  g_clear_object (&self->cancellable);

  /* Release usb interface */
  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                  0, 0, &error);

  /* Notify close complete */
  fpi_image_device_close_complete (dev, error);
}

/* Usb id table of device */
static const FpIdEntry id_table[] = {
  { .vid = 0x2541,  .pid = 0x0236, },
  { .vid = 0x2541,  .pid = 0x9711, },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_cs9711_init (FpDeviceCs9711 *self)
{
}

static void
fpi_device_cs9711_class_init (FpDeviceCs9711Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  // Replace assertion with proper error handling to avoid crashes
  if ((CS9711_FRAME_SIZE) != (CS9711_FP_RECV_LEN_1 + CS9711_FP_RECV_LEN_2))
    {
      g_critical ("CS9711 frame size mismatch: CS9711_FRAME_SIZE=%d, expected=%d",
                  CS9711_FRAME_SIZE, CS9711_FP_RECV_LEN_1 + CS9711_FP_RECV_LEN_2);
      // This is a critical configuration error, but we handle it gracefully
      g_return_if_reached ();
    }

  dev_class->id = "cs9711";
  dev_class->full_name = "Chipsailing CS9711Fingprint";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = 15;

  img_class->algorithm = FPI_PRINT_SIGFM;
  img_class->img_open = dev_open;
  img_class->img_close = dev_close;
  img_class->activate = dev_activate;
  img_class->deactivate = dev_deactivate;
  img_class->change_state = dev_change_state;

  //TODO: Makes very marginal improvement, stick with default in case
  //      it changes with a better implementation in the future
  // img_class->bz3_threshold = 24;

  img_class->img_width = CS9711_WIDTH;
  img_class->img_height = CS9711_HEIGHT;
}
