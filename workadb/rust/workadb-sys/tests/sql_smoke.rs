use std::ffi::CStr;
use std::mem::ManuallyDrop;
use std::path::PathBuf;
use std::sync::{Mutex, Once};

use libc::{c_char, c_int, c_void};

use workadb_sys::{Workadb, WorkadbConfig, wepg_set_logger};

static INIT: Once = Once::new();
static TEST_LOCK: Mutex<()> = Mutex::new(());

#[test]
#[ignore = "PGDATA incompatibility in workspace runs; enable when running workadb tests in isolation"]
fn sql_suite() {
    let _guard = TEST_LOCK.lock().expect("lock tests");
    init_env();
    let (_base_path, pgdata_path, tmp_path) = create_paths();

    eprintln!("workadb test: init");
    let config = WorkadbConfig::new(&pgdata_path, &tmp_path);
    let engine = Workadb::init(&config).expect("init workadb");
    eprintln!("workadb test: init ok");

    let mut case_id = 0u32;

    macro_rules! run_case {
        ($sql:expr) => {{
            case_id += 1;
            let sql = $sql;
            engine.exec_sql(sql).unwrap_or_else(|err| {
                panic!("case {} failed: {}\n{}", case_id, sql, err);
            })
        }};
    }

    macro_rules! expect_err {
        ($sql:expr) => {{
            case_id += 1;
            let sql = $sql;
            if let Ok(frame) = engine.exec_sql(sql) {
                panic!(
                    "case {} expected error but succeeded: {}\n{frame:?}",
                    case_id, sql
                );
            }
        }};
    }

    let frame = run_case!("SELECT 1;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("1"));
    let frame = run_case!("SELECT 2 + 2;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("4"));
    let frame = run_case!("SELECT 'hello'::text;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("hello"));

    run_case!(
        "CREATE TABLE properties (id INT PRIMARY KEY, name TEXT NOT NULL, city TEXT NOT NULL);"
    );
    run_case!("CREATE TABLE tenants (id INT PRIMARY KEY, name TEXT NOT NULL, phone TEXT);");
    run_case!(
        "CREATE TABLE leases (id INT PRIMARY KEY, property_id INT NOT NULL REFERENCES properties(id), tenant_id INT NOT NULL REFERENCES tenants(id), start_date DATE, end_date DATE);"
    );
    run_case!(
        "CREATE TABLE issues (id INT PRIMARY KEY, lease_id INT REFERENCES leases(id), title TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'open', opened_at TIMESTAMPTZ DEFAULT NOW());"
    );
    run_case!(
        "CREATE TABLE payments (id INT PRIMARY KEY, lease_id INT NOT NULL REFERENCES leases(id), amount_cents INT NOT NULL, paid_on DATE);"
    );
    run_case!(
        "CREATE TABLE issue_notes (issue_id INT REFERENCES issues(id), note TEXT NOT NULL, created_at TIMESTAMPTZ DEFAULT NOW(), PRIMARY KEY(issue_id, note));"
    );
    run_case!(
        "CREATE TABLE inspections (id INT PRIMARY KEY, score INT CHECK (score >= 0 AND score <= 100));"
    );
    run_case!("CREATE INDEX issues_status_idx ON issues(status);");
    run_case!("ALTER TABLE issues ADD COLUMN priority INT DEFAULT 1;");
    run_case!("ALTER TABLE issues ADD COLUMN closed_at TIMESTAMPTZ;");

    let frame = run_case!(
        "INSERT INTO properties (id, name, city) VALUES (1, 'Oak Plaza', 'London'), (2, 'Pine House', 'Bristol');"
    );
    assert_eq!(frame.row_count, 2);
    let frame = run_case!(
        "INSERT INTO tenants (id, name, phone) VALUES (10, 'Ava Reed', '555-0100'), (11, 'Noah Hunt', '555-0101');"
    );
    assert_eq!(frame.row_count, 2);
    let frame = run_case!(
        "INSERT INTO leases (id, property_id, tenant_id, start_date, end_date) VALUES (20, 1, 10, '2024-01-01', '2024-12-31'), (21, 2, 11, '2024-06-01', '2025-05-31');"
    );
    assert_eq!(frame.row_count, 2);
    let frame = run_case!(
        "INSERT INTO issues (id, lease_id, title) VALUES (100, 20, 'Sink won''t drain'), (101, 21, 'Heater noisy');"
    );
    assert_eq!(frame.row_count, 2);
    let frame = run_case!(
        "INSERT INTO payments (id, lease_id, amount_cents, paid_on) VALUES (300, 20, 120000, '2025-01-01'), (301, 20, 125000, '2025-02-01'), (302, 21, 110000, '2025-01-15');"
    );
    assert_eq!(frame.row_count, 3);
    let frame = run_case!(
        "INSERT INTO issue_notes (issue_id, note) VALUES (100, 'Tenant reported gurgling'), (101, 'Unit rattles at night');"
    );
    assert_eq!(frame.row_count, 2);
    let frame = run_case!("INSERT INTO inspections (id, score) VALUES (500, 98);");
    assert_eq!(frame.row_count, 1);

    let frame = run_case!("SELECT COUNT(*) FROM properties;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("2"));
    let frame = run_case!("SELECT COUNT(*) FROM tenants;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("2"));
    let frame = run_case!(
        "SELECT l.id, p.name, t.name FROM leases l JOIN properties p ON p.id = l.property_id JOIN tenants t ON t.id = l.tenant_id ORDER BY l.id;"
    );
    assert_eq!(frame.rows.len(), 2);
    let frame = run_case!("SELECT COUNT(*) FROM issues WHERE status = 'open';");
    assert_eq!(frame.rows[0][0].as_deref(), Some("2"));

    let frame = run_case!("UPDATE issues SET status='closed', closed_at=NOW() WHERE id=100;");
    assert_eq!(frame.row_count, 1);
    let frame = run_case!("SELECT COUNT(*) FROM issues WHERE status = 'open';");
    assert_eq!(frame.rows[0][0].as_deref(), Some("1"));
    let frame = run_case!("UPDATE issues SET priority = 2 WHERE status = 'open';");
    assert_eq!(frame.row_count, 1);
    let frame = run_case!("SELECT MAX(priority) FROM issues;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("2"));
    let frame = run_case!(
        "SELECT id, CASE WHEN status='closed' THEN 'done' ELSE 'todo' END FROM issues ORDER BY id;"
    );
    assert_eq!(frame.rows.len(), 2);
    let frame =
        run_case!("SELECT id, COALESCE(closed_at::date::text, 'open') FROM issues ORDER BY id;");
    assert_eq!(frame.rows.len(), 2);
    let frame = run_case!("SELECT id, title FROM issues ORDER BY id DESC LIMIT 1;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("101"));
    let frame = run_case!("SELECT DISTINCT status FROM issues ORDER BY status;");
    assert_eq!(frame.rows.len(), 2);
    let frame = run_case!(
        "SELECT lease_id, COUNT(*) FROM issues GROUP BY lease_id HAVING COUNT(*) >= 1 ORDER BY lease_id;"
    );
    assert_eq!(frame.rows.len(), 2);
    let frame = run_case!(
        "SELECT i.id, n.note FROM issues i LEFT JOIN issue_notes n ON n.issue_id = i.id ORDER BY i.id;"
    );
    assert_eq!(frame.rows.len(), 2);
    let frame = run_case!(
        "SELECT id, amount_cents, SUM(amount_cents) OVER (PARTITION BY lease_id ORDER BY id) FROM payments ORDER BY id;"
    );
    assert_eq!(frame.rows.len(), 3);
    let frame = run_case!(
        "WITH recent AS (SELECT id FROM issues WHERE opened_at >= NOW() - INTERVAL '7 days') SELECT COUNT(*) FROM recent;"
    );
    assert_eq!(frame.rows[0][0].as_deref(), Some("2"));
    let frame = run_case!(
        "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 3) SELECT SUM(n) FROM seq;"
    );
    assert_eq!(frame.rows[0][0].as_deref(), Some("6"));
    let frame = run_case!(
        "INSERT INTO issues (id, lease_id, title) VALUES (102, 20, 'Leaky tap') RETURNING id;"
    );
    assert_eq!(frame.rows[0][0].as_deref(), Some("102"));
    let frame = run_case!("UPDATE issues SET status='in_progress' WHERE id=101 RETURNING status;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("in_progress"));
    let frame = run_case!("DELETE FROM issues WHERE id=102 RETURNING id;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("102"));

    let frame = run_case!(
        "INSERT INTO tenants (id, name) VALUES (10, 'Ava Reed') ON CONFLICT (id) DO NOTHING;"
    );
    assert_eq!(frame.row_count, 0);
    let frame = run_case!(
        "INSERT INTO tenants (id, name) VALUES (11, 'Noah H.') ON CONFLICT (id) DO UPDATE SET name=EXCLUDED.name RETURNING name;"
    );
    assert_eq!(frame.rows[0][0].as_deref(), Some("Noah H."));

    run_case!("CREATE VIEW open_issues AS SELECT id, title FROM issues WHERE status <> 'closed';");
    let frame = run_case!("SELECT COUNT(*) FROM open_issues;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("1"));
    run_case!("DROP VIEW open_issues;");

    run_case!("CREATE TEMP TABLE temp_events (id INT, note TEXT);");
    let frame = run_case!("INSERT INTO temp_events (id, note) VALUES (1, 'temp');");
    assert_eq!(frame.row_count, 1);
    let frame = run_case!("SELECT note FROM temp_events WHERE id=1;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("temp"));
    run_case!("DROP TABLE temp_events;");

    let frame =
        run_case!("INSERT INTO issues (id, lease_id, title) VALUES (103, 20, 'Leak under sink');");
    assert_eq!(frame.row_count, 1);
    let frame = run_case!("UPDATE issues SET title='Leak under sink (edited)' WHERE id=103;");
    assert_eq!(frame.row_count, 1);
    let frame = run_case!("SELECT title FROM issues WHERE id=103;");
    assert_eq!(
        frame.rows[0][0].as_deref(),
        Some("Leak under sink (edited)")
    );

    let frame = run_case!("DELETE FROM issues WHERE id=103;");
    assert_eq!(frame.row_count, 1);
    let frame = run_case!("SELECT COUNT(*) FROM issues WHERE id=103;");
    assert_eq!(frame.rows[0][0].as_deref(), Some("0"));

    let frame = run_case!("SELECT EXISTS (SELECT 1 FROM issues WHERE status <> 'closed');");
    assert_eq!(frame.rows[0][0].as_deref(), Some("t"));
    let frame = run_case!(
        "SELECT id FROM issues WHERE id IN (SELECT issue_id FROM issue_notes) ORDER BY id;"
    );
    assert_eq!(frame.rows.len(), 2);
    let frame = run_case!("SELECT 1 UNION SELECT 2 UNION SELECT 2 ORDER BY 1;");
    assert_eq!(frame.rows.len(), 2);
    let frame = run_case!("SELECT 1 INTERSECT SELECT 1;");
    assert_eq!(frame.rows.len(), 1);
    let frame = run_case!("SELECT 1 EXCEPT SELECT 1;");
    assert_eq!(frame.rows.len(), 0);

    run_case!("SELECT txid_current();");
    run_case!("SELECT pg_backend_pid();");
    run_case!("SELECT COUNT(*) FILTER (WHERE status='open') FROM issues;");
    run_case!("ALTER TABLE issues DROP COLUMN closed_at;");

    expect_err!("INSERT INTO inspections (id, score) VALUES (501, 200);");
    expect_err!("INSERT INTO tenants (id, name) VALUES (10, 'Dup');");

    run_case!("ANALYZE issues;");
    run_case!("DROP TABLE issue_notes;");
    run_case!("CREATE TABLE persisted (id INT PRIMARY KEY, note TEXT);");
    let frame = run_case!("INSERT INTO persisted (id, note) VALUES (1, 'hello');");
    assert_eq!(frame.row_count, 1);
    let _engine = ManuallyDrop::new(engine);
    let reopen_config =
        WorkadbConfig::new(&pgdata_path, &tmp_path).flags(workadb_sys::WEPG_FLAG_NO_AUTO_INITDB);
    let reopen_engine = Workadb::init(&reopen_config).expect("reopen workadb");
    let frame = reopen_engine
        .exec_sql("SELECT note FROM persisted WHERE id=1;")
        .expect("select persisted");
    assert_eq!(frame.rows[0][0].as_deref(), Some("hello"));

    assert!(case_id >= 50, "expected at least 50 cases, got {case_id}");

    eprintln!("workadb test: shutdown");
    reopen_engine.shutdown().expect("shutdown workadb");
}

fn init_env() {
    INIT.call_once(|| {
        if std::env::var("WORKADB_TEMPLATE_PATH").is_err() {
            let template = default_template_path().unwrap_or_else(|| {
                panic!("WORKADB_TEMPLATE_PATH not set and default template not found")
            });
            unsafe {
                std::env::set_var("WORKADB_TEMPLATE_PATH", template);
            }
        }
        if std::env::var("WORKADB_EXEC_PATH").is_err() {
            let exec_path = default_exec_path().unwrap_or_else(|| {
                panic!("WORKADB_EXEC_PATH not set and default exec path not found")
            });
            unsafe {
                std::env::set_var("WORKADB_EXEC_PATH", exec_path);
            }
        }
        if std::env::var("WORKADB_DEBUG").is_err() {
            unsafe {
                std::env::set_var("WORKADB_DEBUG", "1");
            }
        }

        unsafe {
            wepg_set_logger(Some(workadb_logger), std::ptr::null_mut());
        }
    });
}

fn create_paths() -> (PathBuf, PathBuf, PathBuf) {
    let temp_root = tempfile::tempdir().expect("tempdir");
    let base_path = temp_root.keep();
    let pgdata_path = base_path.join("pgdata");
    let tmp_path = base_path.join("tmp");
    std::fs::create_dir_all(&tmp_path).expect("tmp dir");
    (base_path, pgdata_path, tmp_path)
}

unsafe extern "C" fn workadb_logger(level: c_int, msg: *const c_char, _ctx: *mut c_void) {
    if msg.is_null() {
        return;
    }
    let message = unsafe { CStr::from_ptr(msg) }.to_string_lossy();
    eprintln!("[workadb:{level}] {message}");
}

fn default_template_path() -> Option<PathBuf> {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let repo_root = find_repo_root(&manifest_dir)?;
    let arch = match std::env::consts::ARCH {
        "aarch64" => "arm64",
        other => other,
    };
    let dist_tag = if cfg!(target_os = "macos") {
        "desktop"
    } else if cfg!(target_os = "linux") {
        "desktop"
    } else {
        "desktop"
    };
    let template = repo_root
        .join("workadb")
        .join("build")
        .join(format!("dist-{dist_tag}"))
        .join(arch)
        .join("share/workadb/pgdata-template");
    if template.exists() {
        Some(template)
    } else {
        None
    }
}

fn default_exec_path() -> Option<PathBuf> {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let repo_root = find_repo_root(&manifest_dir)?;
    let arch = match std::env::consts::ARCH {
        "aarch64" => "arm64",
        other => other,
    };
    let dist_tag = if cfg!(target_os = "macos") {
        "desktop"
    } else if cfg!(target_os = "linux") {
        "desktop"
    } else {
        "desktop"
    };
    let exec_path = repo_root
        .join("workadb")
        .join("build")
        .join(format!("dist-{dist_tag}"))
        .join(arch)
        .join("bin/postgres");
    if exec_path.exists() {
        Some(exec_path)
    } else {
        None
    }
}

fn find_repo_root(start: &PathBuf) -> Option<PathBuf> {
    let mut current = Some(start.as_path());
    while let Some(dir) = current {
        if dir.join("workadb").is_dir() && dir.join("src").is_dir() {
            return Some(dir.to_path_buf());
        }
        current = dir.parent();
    }
    None
}
