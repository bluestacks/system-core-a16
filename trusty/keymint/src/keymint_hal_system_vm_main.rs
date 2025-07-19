//
// Copyright (C) 2025 The Android Open-Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! This module implements the HAL service for Keymint (Rust) interacting with
//! the Trusty VM.

use android_keymint_trusty_commservice::aidl::android::keymint::trusty::commservice::ICommService::ICommService;
use anyhow::{anyhow, bail, Context, Result};
use binder::{self, AccessorProvider, ProcessState, Strong};
use kmr_hal::{keymint, rpc, secureclock, send_hal_info, sharedsecret, SerializedChannel};
use log::{error, info, warn};
use std::{
    ops::DerefMut,
    panic,
    sync::{Arc, Mutex},
    thread,
    time::Duration,
};

const SERVICE_INSTANCE: &str = "default";

const KM_SERVICE_NAME: &str = "android.hardware.security.keymint.IKeyMintDevice";
const RPC_SERVICE_NAME: &str = "android.hardware.security.keymint.IRemotelyProvisionedComponent";
const SECURE_CLOCK_SERVICE_NAME: &str = "android.hardware.security.secureclock.ISecureClock";
const SHARED_SECRET_SERVICE_NAME: &str = "android.hardware.security.sharedsecret.ISharedSecret";

const ACCESSOR_SERVICE_NAME: &str = "android.os.IAccessor/ICommService/default";
const INTERNAL_RPC_SERVICE_NAME: &str = "android.keymint.trusty.commservice.ICommService/default";

#[derive(Debug)]
struct CommServiceChannel {
    comm_service: Strong<dyn ICommService>,
}

impl SerializedChannel for CommServiceChannel {
    const MAX_SIZE: usize = 4000;
    fn execute(&mut self, serialized_req: &[u8]) -> binder::Result<Vec<u8>> {
        self.comm_service.execute_transact(serialized_req)
    }
}

/// Helper struct to provide convenient access to the locked channel.
struct HalChannel(Arc<Mutex<CommServiceChannel>>);

impl HalChannel {
    /// Executes a closure with a mutable reference to the inner channel.
    fn with<F, R>(&self, f: F) -> Result<R>
    where
        F: FnOnce(&mut CommServiceChannel) -> Result<R>,
    {
        let mut channel = self.0.lock().map_err(|_| anyhow!("Mutex was poisoned"))?;
        f(channel.deref_mut())
    }
}

impl From<CommServiceChannel> for HalChannel {
    fn from(channel: CommServiceChannel) -> Self {
        Self(Arc::new(Mutex::new(channel)))
    }
}

fn main() {
    if let Err(e) = inner_main() {
        panic!("HAL service failed: {:?}", e);
    }
}

fn inner_main() -> Result<()> {
    setup_logging_and_panic_hook();

    if cfg!(feature = "nonsecure") {
        warn!("Non-secure Trusty KM HAL service is starting.");
    } else {
        info!("Trusty KM HAL service is starting.");
    }

    info!("Starting thread pool.");
    ProcessState::start_thread_pool();

    // TODO(b/429217397): Use a proper way to register an accessor and get the internal RPC
    // service via accessor here.
    let _accessor_provider = AccessorProvider::new(&[INTERNAL_RPC_SERVICE_NAME.to_owned()], |s| {
        binder::wait_for_service(ACCESSOR_SERVICE_NAME)
            .and_then(|service| binder::Accessor::from_binder(s, service))
    })
    .ok_or(anyhow!("failed to create accessor provider"))?;
    let comm_service = get_comm_service_with_retry()?;
    info!("Connected to ICommService.");
    let channel: HalChannel = CommServiceChannel { comm_service }.into();

    #[cfg(feature = "nonsecure")]
    {
        // When the non-secure feature is enabled, retrieve root-of-trust information
        // (with the exception of the verified boot key hash) from Android properties, and
        // populate the TA with this information. On a real device, the bootloader should
        // provide this data to the TA directly.
        let boot_req = kmr_hal_nonsecure::get_boot_info();
        info!("boot/HAL->TA: boot info is {:?}", boot_req);
        channel
            .with(|c| kmr_hal::send_boot_info(c, boot_req).context("failed to send boot info"))?;

        // When the non-secure feature is enabled, also retrieve device ID information
        // (except for IMEI/MEID values) from Android properties and populate the TA with
        // this information. On a real device, a factory provisioning process would populate
        // this information.
        let attest_ids = kmr_hal_nonsecure::attestation_id_info();
        if let Err(e) = channel.with(|c| {
            kmr_hal::send_attest_ids(c, attest_ids).context("failed to send attestation ID")
        }) {
            error!("failed to send attestation ID info: {:?}", e);
        }
        info!("Successfully sent non-secure boot info and attestation IDs to the TA.");
    }

    register_keymint_services(&channel.0)?;

    // Send the HAL service information to the TA
    channel.with(|c| send_hal_info(c).context("failed to populate HAL info"))?;

    info!("Successfully registered KeyMint HAL services. Joining thread pool now.");

    ProcessState::join_thread_pool();
    bail!("Binder thread pool exited unexpectedly, terminating HAL service.");
}

/// Gets the ICommService binder interface, retrying on failure.
fn get_comm_service_with_retry() -> Result<Strong<dyn ICommService>> {
    const MAX_ATTEMPTS: u32 = 5;
    const RETRY_DELAY: Duration = Duration::from_secs(1);

    for attempt in 1..MAX_ATTEMPTS {
        match binder::get_interface(INTERNAL_RPC_SERVICE_NAME) {
            Ok(service) => return Ok(service),
            Err(e) => {
                warn!(
                    "Attempt {}/{} to get ICommService failed: {}. Retrying in {:?}...",
                    attempt, MAX_ATTEMPTS, e, RETRY_DELAY
                );
                thread::sleep(RETRY_DELAY);
            }
        }
    }
    binder::get_interface(INTERNAL_RPC_SERVICE_NAME)
        .with_context(|| format!("failed to get ICommService after {} attempts", MAX_ATTEMPTS))
}

fn setup_logging_and_panic_hook() {
    android_logger::init_once(
        android_logger::Config::default()
            .with_tag("keymint-hal-trusty-vm")
            .with_max_level(log::LevelFilter::Info)
            .with_log_buffer(android_logger::LogId::System),
    );
    // In case of a panic, log it before the process terminates.
    panic::set_hook(Box::new(|panic_info| {
        error!("PANIC: {}", panic_info);
    }));
}

fn register_keymint_services(channel: &Arc<Mutex<CommServiceChannel>>) -> Result<()> {
    /// Helper to register a single HAL service.
    fn register_hal<F, T>(
        base_name: &str,
        channel: &Arc<Mutex<CommServiceChannel>>,
        constructor: F,
    ) -> Result<()>
    where
        F: FnOnce(Arc<Mutex<CommServiceChannel>>) -> Strong<T>,
        T: binder::FromIBinder + ?Sized,
    {
        let service = constructor(channel.clone());
        let full_name = format!("{}/{}", base_name, SERVICE_INSTANCE);
        binder::add_service(&full_name, service.as_binder())
            .with_context(|| format!("failed to add service {full_name}"))?;
        info!("Registered Binder service {full_name}.");
        Ok(())
    }

    register_hal(KM_SERVICE_NAME, channel, keymint::Device::new_as_binder)?;
    register_hal(RPC_SERVICE_NAME, channel, rpc::Device::new_as_binder)?;
    register_hal(SECURE_CLOCK_SERVICE_NAME, channel, secureclock::Device::new_as_binder)?;
    register_hal(SHARED_SECRET_SERVICE_NAME, channel, sharedsecret::Device::new_as_binder)?;

    Ok(())
}
