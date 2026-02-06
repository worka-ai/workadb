#![allow(non_camel_case_types)]

use libc::{c_char, c_int, c_void, size_t};
use std::ffi::{CStr, CString};
use std::path::{Path, PathBuf};
use std::ptr;

pub const WEPG_ABI_MAJOR: u32 = 1;
pub const WEPG_ABI_MINOR: u32 = 2;

#[repr(C)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum wepg_status {
    WEPG_OK = 0,
    WEPG_ERR_INVALID_CONFIG = 1,
    WEPG_ERR_INITDB_FAILED = 2,
    WEPG_ERR_EXEC_FAILED = 3,
    WEPG_ERR_BUFFER_TOO_SMALL = 4,
    WEPG_ERR_INCOMPATIBLE_PGDATA = 5,
    WEPG_ERR_UPGRADE_REQUIRED = 6,
    WEPG_ERR_UNSUPPORTED = 7,
    WEPG_ERR_INTERNAL = 8,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum wepg_mode {
    WEPG_MODE_SQL = 1,
    WEPG_MODE_WIRE = 2,
}

pub const WEPG_FLAG_ENABLE_WIRE: u32 = 1 << 0;
pub const WEPG_FLAG_READONLY: u32 = 1 << 1;
pub const WEPG_FLAG_DISABLE_DYNAMIC_EXT: u32 = 1 << 2;
pub const WEPG_FLAG_NO_AUTO_INITDB: u32 = 1 << 3;

#[repr(C)]
pub struct wepg_engine {
    _private: [u8; 0],
}

pub type wepg_handle = *mut wepg_engine;

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct wepg_config {
    pub pgdata_path: *const c_char,
    pub temp_path: *const c_char,
    pub flags: u32,
    pub max_response_bytes: u32,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct wepg_request {
    pub mode: wepg_mode,
    pub bytes: *const u8,
    pub len: size_t,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct wepg_param {
    pub type_oid: u32,
    pub value: *const u8,
    pub len: size_t,
    pub is_null: u8,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct wepg_request_params {
    pub mode: wepg_mode,
    pub bytes: *const u8,
    pub len: size_t,
    pub params: *const wepg_param,
    pub param_count: size_t,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct wepg_response {
    pub bytes: *mut u8,
    pub len: size_t,
    pub capacity: size_t,
    pub rows: i64,
}

pub type wepg_log_fn =
    Option<unsafe extern "C" fn(level: c_int, msg: *const c_char, ctx: *mut c_void)>;

unsafe extern "C" {
    pub fn wepg_abi_version() -> u32;
    pub fn wepg_version_string() -> *const c_char;
    pub fn wepg_set_logger(fn_ptr: wepg_log_fn, ctx: *mut c_void);

    pub fn wepg_init(config: *const wepg_config, out: *mut wepg_handle) -> wepg_status;
    pub fn wepg_initdb(handle: wepg_handle) -> wepg_status;
    pub fn wepg_step(
        handle: wepg_handle,
        request: *const wepg_request,
        response: *mut wepg_response,
    ) -> wepg_status;
    pub fn wepg_step_params(
        handle: wepg_handle,
        request: *const wepg_request_params,
        response: *mut wepg_response,
    ) -> wepg_status;
    pub fn wepg_reset(handle: wepg_handle) -> wepg_status;
    pub fn wepg_shutdown(handle: wepg_handle) -> wepg_status;
    pub fn wepg_last_error(handle: wepg_handle) -> *const c_char;
    pub fn workadb_assets_install(dir_path: *const c_char) -> wepg_status;
    pub fn workadb_assets_last_error() -> *const c_char;
    pub fn workadb_assets_version_string() -> *const c_char;
}

#[derive(Debug)]
pub struct WorkadbConfig {
    pgdata_path: PathBuf,
    temp_path: PathBuf,
    flags: u32,
    max_response_bytes: usize,
}

impl WorkadbConfig {
    pub fn new(pgdata_path: impl AsRef<Path>, temp_path: impl AsRef<Path>) -> Self {
        Self {
            pgdata_path: pgdata_path.as_ref().to_path_buf(),
            temp_path: temp_path.as_ref().to_path_buf(),
            flags: 0,
            max_response_bytes: 512 * 1024,
        }
    }

    pub fn flags(mut self, flags: u32) -> Self {
        self.flags = flags;
        self
    }

    pub fn max_response_bytes(mut self, max_response_bytes: usize) -> Self {
        self.max_response_bytes = max_response_bytes;
        self
    }
}

#[derive(Debug)]
pub struct Workadb {
    handle: wepg_handle,
    max_response_bytes: usize,
    _pgdata_c: CString,
    _temp_c: CString,
}

#[derive(Debug, Clone)]
pub struct WorkadbParam {
    pub type_oid: u32,
    pub value: Option<String>,
}

impl WorkadbParam {
    pub fn new(type_oid: u32, value: Option<String>) -> Self {
        Self { type_oid, value }
    }
}

impl Workadb {
    pub fn install_assets(dir_path: impl AsRef<Path>) -> Result<(), WorkadbError> {
        let path_c = CString::new(dir_path.as_ref().to_string_lossy().as_bytes())
            .map_err(|_| WorkadbError::new(wepg_status::WEPG_ERR_INVALID_CONFIG, None))?;
        let status = unsafe { workadb_assets_install(path_c.as_ptr()) };
        if status != wepg_status::WEPG_OK {
            let msg = unsafe { workadb_assets_last_error() };
            let detail = if msg.is_null() {
                None
            } else {
                Some(unsafe { CStr::from_ptr(msg) }.to_string_lossy().to_string())
            };
            return Err(WorkadbError::new(status, detail));
        }
        Ok(())
    }

    pub fn assets_version() -> Option<String> {
        let msg = unsafe { workadb_assets_version_string() };
        if msg.is_null() {
            None
        } else {
            Some(unsafe { CStr::from_ptr(msg) }.to_string_lossy().to_string())
        }
    }

    pub fn init(config: &WorkadbConfig) -> Result<Self, WorkadbError> {
        let pgdata_c = CString::new(config.pgdata_path.to_string_lossy().as_bytes())
            .map_err(|_| WorkadbError::new(wepg_status::WEPG_ERR_INVALID_CONFIG, None))?;
        let temp_c = CString::new(config.temp_path.to_string_lossy().as_bytes())
            .map_err(|_| WorkadbError::new(wepg_status::WEPG_ERR_INVALID_CONFIG, None))?;

        let cfg = wepg_config {
            pgdata_path: pgdata_c.as_ptr(),
            temp_path: temp_c.as_ptr(),
            flags: config.flags,
            max_response_bytes: config.max_response_bytes as u32,
        };

        let mut handle: wepg_handle = ptr::null_mut();
        let status = unsafe { wepg_init(&cfg, &mut handle) };
        if status != wepg_status::WEPG_OK {
            return Err(WorkadbError::from_status(status, handle));
        }

        Ok(Self {
            handle,
            max_response_bytes: config.max_response_bytes,
            _pgdata_c: pgdata_c,
            _temp_c: temp_c,
        })
    }

    pub fn initdb(&self) -> Result<(), WorkadbError> {
        let status = unsafe { wepg_initdb(self.handle) };
        if status != wepg_status::WEPG_OK {
            return Err(WorkadbError::from_status(status, self.handle));
        }
        Ok(())
    }

    pub fn exec_sql(&self, sql: &str) -> Result<WorkadbFrame, WorkadbError> {
        let response = self.exec_sql_raw(sql)?;
        WorkadbFrame::decode(&response.bytes, response.rows)
            .map_err(|err| WorkadbError::new(wepg_status::WEPG_ERR_INTERNAL, Some(err)))
    }

    pub fn exec_sql_params(
        &self,
        sql: &str,
        params: &[WorkadbParam],
    ) -> Result<WorkadbFrame, WorkadbError> {
        let response = self.exec_sql_params_raw(sql, params)?;
        WorkadbFrame::decode(&response.bytes, response.rows)
            .map_err(|err| WorkadbError::new(wepg_status::WEPG_ERR_INTERNAL, Some(err)))
    }

    pub fn exec_sql_raw(&self, sql: &str) -> Result<WorkadbResponse, WorkadbError> {
        let mut buffer = vec![0u8; self.max_response_bytes];
        let mut response = wepg_response {
            bytes: buffer.as_mut_ptr(),
            len: 0,
            capacity: buffer.len(),
            rows: 0,
        };
        let request = wepg_request {
            mode: wepg_mode::WEPG_MODE_SQL,
            bytes: sql.as_bytes().as_ptr(),
            len: sql.len(),
        };

        let status = unsafe { wepg_step(self.handle, &request, &mut response) };
        if status != wepg_status::WEPG_OK {
            return Err(WorkadbError::from_status(status, self.handle));
        }

        let bytes = unsafe { std::slice::from_raw_parts(response.bytes, response.len) }.to_vec();
        Ok(WorkadbResponse {
            bytes,
            rows: response.rows,
        })
    }

    pub fn exec_sql_params_raw(
        &self,
        sql: &str,
        params: &[WorkadbParam],
    ) -> Result<WorkadbResponse, WorkadbError> {
        if params.is_empty() {
            return self.exec_sql_raw(sql);
        }

        let mut buffer = vec![0u8; self.max_response_bytes];
        let mut response = wepg_response {
            bytes: buffer.as_mut_ptr(),
            len: 0,
            capacity: buffer.len(),
            rows: 0,
        };
        let mut raw_params = Vec::with_capacity(params.len());
        for param in params {
            match &param.value {
                Some(value) => raw_params.push(wepg_param {
                    type_oid: param.type_oid,
                    value: value.as_bytes().as_ptr(),
                    len: value.len(),
                    is_null: 0,
                }),
                None => raw_params.push(wepg_param {
                    type_oid: param.type_oid,
                    value: ptr::null(),
                    len: 0,
                    is_null: 1,
                }),
            }
        }
        let request = wepg_request_params {
            mode: wepg_mode::WEPG_MODE_SQL,
            bytes: sql.as_bytes().as_ptr(),
            len: sql.len(),
            params: raw_params.as_ptr(),
            param_count: raw_params.len(),
        };

        let status = unsafe { wepg_step_params(self.handle, &request, &mut response) };
        if status != wepg_status::WEPG_OK {
            return Err(WorkadbError::from_status(status, self.handle));
        }

        let bytes = unsafe { std::slice::from_raw_parts(response.bytes, response.len) }.to_vec();
        Ok(WorkadbResponse {
            bytes,
            rows: response.rows,
        })
    }

    pub fn reset(&self) -> Result<(), WorkadbError> {
        let status = unsafe { wepg_reset(self.handle) };
        if status != wepg_status::WEPG_OK {
            return Err(WorkadbError::from_status(status, self.handle));
        }
        Ok(())
    }

    pub fn shutdown(mut self) -> Result<(), WorkadbError> {
        if self.handle.is_null() {
            return Ok(());
        }
        let status = unsafe { wepg_shutdown(self.handle) };
        self.handle = ptr::null_mut();
        if status != wepg_status::WEPG_OK {
            return Err(WorkadbError::from_status(status, ptr::null_mut()));
        }
        Ok(())
    }
}

impl Drop for Workadb {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe {
                let _ = wepg_shutdown(self.handle);
            }
            self.handle = ptr::null_mut();
        }
    }
}

#[derive(Debug)]
pub struct WorkadbResponse {
    pub bytes: Vec<u8>,
    pub rows: i64,
}

#[derive(Debug)]
pub struct WorkadbFrame {
    pub version: u32,
    pub columns: Vec<WorkadbColumn>,
    pub rows: Vec<Vec<Option<String>>>,
    pub row_count: i64,
}

#[derive(Debug)]
pub struct WorkadbColumn {
    pub name: String,
    pub type_oid: u32,
    pub type_len: i16,
    pub type_mod: i32,
}

impl WorkadbFrame {
    pub fn decode(buf: &[u8], rows: i64) -> Result<Self, String> {
        let mut idx = 0usize;
        let version = read_u32(buf, &mut idx)?;
        let col_count = read_u32(buf, &mut idx)? as usize;
        let row_count = read_u32(buf, &mut idx)? as usize;

        let mut columns = Vec::with_capacity(col_count);
        for _ in 0..col_count {
            let name_len = read_u32(buf, &mut idx)? as usize;
            let name = read_string(buf, &mut idx, name_len)?;
            let type_oid = read_u32(buf, &mut idx)?;
            let type_len = read_i16(buf, &mut idx)?;
            let type_mod = read_i32(buf, &mut idx)?;
            columns.push(WorkadbColumn {
                name,
                type_oid,
                type_len,
                type_mod,
            });
        }

        let mut rows_out = Vec::with_capacity(row_count);
        for _ in 0..row_count {
            let mut row = Vec::with_capacity(col_count);
            for _ in 0..col_count {
                let val_len = read_i32(buf, &mut idx)?;
                if val_len < 0 {
                    row.push(None);
                } else {
                    let value = read_string(buf, &mut idx, val_len as usize)?;
                    row.push(Some(value));
                }
            }
            rows_out.push(row);
        }

        Ok(Self {
            version,
            columns,
            rows: rows_out,
            row_count: rows,
        })
    }
}

#[derive(Debug)]
pub struct WorkadbError {
    pub status: wepg_status,
    pub message: Option<String>,
}

impl WorkadbError {
    fn new(status: wepg_status, message: Option<String>) -> Self {
        Self { status, message }
    }

    fn from_status(status: wepg_status, handle: wepg_handle) -> Self {
        let message = if handle.is_null() {
            None
        } else {
            let msg_ptr = unsafe { wepg_last_error(handle) };
            if msg_ptr.is_null() {
                None
            } else {
                Some(
                    unsafe { CStr::from_ptr(msg_ptr) }
                        .to_string_lossy()
                        .into_owned(),
                )
            }
        };
        Self { status, message }
    }
}

impl std::fmt::Display for WorkadbError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match &self.message {
            Some(msg) => write!(f, "{:?}: {}", self.status, msg),
            None => write!(f, "{:?}", self.status),
        }
    }
}

impl std::error::Error for WorkadbError {}

fn read_u32(buf: &[u8], idx: &mut usize) -> Result<u32, String> {
    if *idx + 4 > buf.len() {
        return Err("buffer underrun".to_string());
    }
    let value = ((buf[*idx] as u32) << 24)
        | ((buf[*idx + 1] as u32) << 16)
        | ((buf[*idx + 2] as u32) << 8)
        | (buf[*idx + 3] as u32);
    *idx += 4;
    Ok(value)
}

fn read_i32(buf: &[u8], idx: &mut usize) -> Result<i32, String> {
    Ok(read_u32(buf, idx)? as i32)
}

fn read_i16(buf: &[u8], idx: &mut usize) -> Result<i16, String> {
    if *idx + 2 > buf.len() {
        return Err("buffer underrun".to_string());
    }
    let value = ((buf[*idx] as i16) << 8) | (buf[*idx + 1] as i16);
    *idx += 2;
    Ok(value)
}

fn read_string(buf: &[u8], idx: &mut usize, len: usize) -> Result<String, String> {
    if *idx + len > buf.len() {
        return Err("buffer underrun".to_string());
    }
    let out = String::from_utf8(buf[*idx..*idx + len].to_vec())
        .map_err(|_| "invalid utf8".to_string())?;
    *idx += len;
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn abi_version_is_nonzero() {
        let version = unsafe { wepg_abi_version() };
        assert!(version >= WEPG_ABI_MAJOR);
    }
}
