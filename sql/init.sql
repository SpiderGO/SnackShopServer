DROP TABLE IF EXISTS products;

CREATE TABLE products(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    category TEXT NOT NULL,
    price REAL NOT NULL,
    stock INTEGER NOT NULL,
    main_image_url TEXT NOT NULL DEFAULT '',
    enabled INTEGER NOT NULL DEFAULT 1
);

INSERT INTO products (name, category, price, stock, enabled) VALUES
('香辣薯片', '膨化食品', 6.5, 20, 1),
('无糖可乐', '饮料', 4.0, 30, 1),
('魔芋爽', '辣味零食', 2.5, 50, 1);