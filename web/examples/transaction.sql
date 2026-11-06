CREATE TABLE accounts (id INT PRIMARY KEY, owner TEXT, balance INT CHECK (balance >= 0));
INSERT INTO accounts VALUES (1, 'Alice', 100);
INSERT INTO accounts VALUES (2, 'Bob', 20);
BEGIN;
UPDATE accounts SET balance = balance - 70 WHERE id = 1;
UPDATE accounts SET balance = balance + 70 WHERE id = 2;
SELECT * FROM accounts;
ROLLBACK;
SELECT * FROM accounts;
