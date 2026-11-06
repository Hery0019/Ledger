CREATE TABLE customers (id INT PRIMARY KEY, name TEXT NOT NULL, city TEXT);
CREATE TABLE orders (
  id          INT PRIMARY KEY AUTOINCREMENT,
  customer_id INT NOT NULL REFERENCES customers(id) ON DELETE CASCADE,
  amount      FLOAT NOT NULL CHECK (amount > 0)
);
INSERT INTO customers VALUES (1, 'Alice', 'Antananarivo');
INSERT INTO customers VALUES (2, 'Bob', 'Toamasina');
INSERT INTO customers VALUES (3, 'Chloé', 'Antananarivo');
INSERT INTO orders (customer_id, amount) VALUES (1, 120.0);
INSERT INTO orders (customer_id, amount) VALUES (1, 80.5);
INSERT INTO orders (customer_id, amount) VALUES (2, 42.0);
SELECT c.name, COUNT(o.id) AS orders, COALESCE(SUM(o.amount), 0) AS total
FROM customers AS c LEFT JOIN orders AS o ON o.customer_id = c.id
GROUP BY c.name
ORDER BY total DESC;
SELECT city, COUNT(*) AS customers FROM customers GROUP BY city HAVING COUNT(*) > 1;
