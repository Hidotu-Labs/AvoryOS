//! USB mass-storage class driver (Phase 6 of docs/usb-storage-plan.md).
//!
//! Layers: [`bot`] frames commands (CBW/CSW, reset recovery), [`scsi`] builds
//! the SCSI commands the disk needs, and [`device`] owns one LUN (start-up,
//! quirks, sector I/O split to the command limit).  The kernel glue in `glue`
//! runs the slot table and worker thread and registers the disk through the
//! block bridge.

pub mod bot;
pub mod device;
pub mod scsi;

#[cfg(not(test))]
mod glue;

#[cfg(test)]
mod tests;

#[cfg(not(test))]
pub use glue::write_diag;

/// Host tests have no usb.c; the diag hook is a no-op there.
#[cfg(test)]
pub fn write_diag(_raw: crate::usb::kernel::Handle) {}
