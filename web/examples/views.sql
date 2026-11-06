CREATE TABLE products (id INT PRIMARY KEY, name TEXT, price FLOAT, stock INT);
INSERT INTO products VALUES (1, 'Keyboard', 49.9, 12);
INSERT INTO products VALUES (2, 'Mouse', 19.5, 0);
INSERT INTO products VALUES (3, 'Monitor', 189.0, 3);
INSERT INTO products VALUES (4, 'Cable', 4.2, 250);
CREATE VIEW in_stock AS SELECT name, price, stock FROM products WHERE stock > 0;
SELECT name,
       CASE WHEN stock > 100 THEN 'plenty' WHEN stock > 5 THEN 'ok' ELSE 'low' END AS level,
       ROUND(price * 1.2, 2) AS price_with_tax
FROM in_stock
ORDER BY price DESC;
SELECT name FROM products WHERE price > (SELECT AVG(price) FROM products);
