CREATE TABLE members (id INT PRIMARY KEY, email TEXT UNIQUE NOT NULL, age INT CHECK (age >= 18));
INSERT INTO members VALUES (1, 'alice@example.com', 30);
INSERT INTO members VALUES (2, 'alice@example.com', 25);
